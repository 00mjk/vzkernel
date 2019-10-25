#include <linux/init.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/ctype.h>
#include <linux/umh.h>
#include "dm-ploop.h"

#define DM_MSG_PREFIX "ploop"

static void ploop_queue_deferred_cmd(struct ploop *ploop, struct ploop_cmd *cmd)
{
	unsigned long flags;

	spin_lock_irqsave(&ploop->deferred_lock, flags);
	BUG_ON(ploop->deferred_cmd && ploop->deferred_cmd != cmd);
	ploop->deferred_cmd = cmd;
	spin_unlock_irqrestore(&ploop->deferred_lock, flags);
	queue_work(ploop->wq, &ploop->worker);
}

/*
 * Assign newly allocated memory for BAT array and holes_bitmap
 * before grow.
 */
static void ploop_advance_bat_and_holes(struct ploop *ploop,
					struct ploop_cmd *cmd)
{
	unsigned int i, size, dst_cluster;

	/* This is called only once */
	if (cmd->resize.stage != PLOOP_GROW_STAGE_INITIAL)
		return;
	cmd->resize.stage++;

	write_lock_irq(&ploop->bat_rwlock);
	/* Copy and swap holes_bitmap */
	size = DIV_ROUND_UP(ploop->hb_nr, 8);
	memcpy(cmd->resize.holes_bitmap, ploop->holes_bitmap, size);
	swap(cmd->resize.holes_bitmap, ploop->holes_bitmap);
	for (i = ploop->hb_nr; i < size * 8; i++)
		set_bit(i, ploop->holes_bitmap);
	swap(cmd->resize.hb_nr, ploop->hb_nr);
	for (i = 0; i < ploop->nr_bat_entries; i++) {
		if (!cluster_is_in_top_delta(ploop, i))
			continue;
		dst_cluster = ploop->bat_entries[i];
		if (dst_cluster < ploop->hb_nr &&
		    test_bit(dst_cluster, ploop->holes_bitmap)) {
			/* This may happen after grow->shrink->(now) grow */
			ploop_hole_clear_bit(dst_cluster, ploop);
		}
	}

	/* Copy and swap bat_entries */
	size = (PLOOP_MAP_OFFSET + ploop->nr_bat_entries) * sizeof(map_index_t);
	memcpy(cmd->resize.hdr, ploop->hdr, size);
	swap(cmd->resize.hdr, ploop->hdr);
	ploop->bat_entries = (void *)ploop->hdr + sizeof(*ploop->hdr);

	/* Copy and swap bat_levels */
	size = ploop->nr_bat_entries * sizeof(ploop->bat_levels[0]);
	memcpy(cmd->resize.bat_levels, ploop->bat_levels, size);
	swap(cmd->resize.bat_levels, ploop->bat_levels);
	write_unlock_irq(&ploop->bat_rwlock);
}

/*
 * Switch index of ploop->inflight_bios_ref[] and wait till inflight
 * bios are completed. This waits for completion of simple submitted
 * action like write to origin_dev or read from delta, but it never
 * guarantees completion of complex actions like "data write + index
 * writeback" (for index protection look at cluster locks). This is
 * weaker, than "dmsetup suspend".
 * It is called from kwork only, so this can't be executed in parallel.
 */
void ploop_inflight_bios_ref_switch(struct ploop *ploop)
{
	unsigned int index = ploop->inflight_bios_ref_index;

	WARN_ON_ONCE(!(current->flags & PF_WQ_WORKER));
	init_completion(&ploop->inflight_bios_ref_comp);

	write_lock_irq(&ploop->bat_rwlock);
	ploop->inflight_bios_ref_index = !index;
	write_unlock_irq(&ploop->bat_rwlock);

	percpu_ref_kill(&ploop->inflight_bios_ref[index]);

	wait_for_completion(&ploop->inflight_bios_ref_comp);
	percpu_ref_reinit(&ploop->inflight_bios_ref[index]);
}

/* Find existing BAT cluster pointing to dst_cluster */
static unsigned int ploop_find_bat_entry(struct ploop *ploop,
					 unsigned int dst_cluster,
					 bool *is_locked)
{
	unsigned int i, cluster = UINT_MAX;

	read_lock_irq(&ploop->bat_rwlock);
	for (i = 0; i < ploop->nr_bat_entries; i++) {
		if (ploop->bat_entries[i] != dst_cluster)
			continue;
		if (cluster_is_in_top_delta(ploop, i)) {
			cluster = i;
			break;
		}
	}
	read_unlock_irq(&ploop->bat_rwlock);

	*is_locked = false;
	if (cluster != UINT_MAX) {
		spin_lock_irq(&ploop->deferred_lock);
		*is_locked = find_lk_of_cluster(ploop, cluster);
		spin_unlock_irq(&ploop->deferred_lock);
	}

	return cluster;
}

void bio_prepare_offsets(struct ploop *ploop, struct bio *bio,
			 unsigned int cluster)
{
	unsigned int cluster_log = ploop->cluster_log;
	int i, nr_pages = nr_pages_in_cluster(ploop);

	bio->bi_vcnt = nr_pages;

	for (i = 0; i < nr_pages; i++) {
		bio->bi_io_vec[i].bv_offset = 0;
		bio->bi_io_vec[i].bv_len = PAGE_SIZE;
	}
	bio->bi_iter.bi_sector = cluster << cluster_log;
	bio->bi_iter.bi_size = 1 << (cluster_log + 9);
}

int ploop_read_cluster_sync(struct ploop *ploop, struct bio *bio,
			    unsigned int cluster)
{
	bio_reset(bio);
	bio_prepare_offsets(ploop, bio, cluster);
	remap_to_origin(ploop, bio);
	bio_set_op_attrs(bio, REQ_OP_READ, 0);

	return submit_bio_wait(bio);
}

static int ploop_write_cluster_sync(struct ploop *ploop, struct bio *bio,
				   unsigned int cluster)
{
	struct block_device *bdev = ploop->origin_dev->bdev;
	int ret;

	bio_reset(bio);
	bio_prepare_offsets(ploop, bio, cluster);
	remap_to_origin(ploop, bio);
	bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_FUA | REQ_PREFLUSH);

	ret = submit_bio_wait(bio);
	track_bio(ploop, bio);
	if (ret)
		return ret;

	if (!blk_queue_fua(bdev_get_queue(bdev))) {
		/*
		 * Error here does not mean that cluster write is failed,
		 * since ploop_map() could submit more bios in parallel.
		 * But it's not possible to differ them. Should we block
		 * ploop_map() during we do this?
		 */
		ret = blkdev_issue_flush(bdev, GFP_NOIO, NULL);
	}

	return ret;
}

static int ploop_write_zero_cluster_sync(struct ploop *ploop,
					 struct bio *bio,
					 unsigned int cluster)
{
	bio_reset(bio);
	bio_prepare_offsets(ploop, bio, cluster);
	zero_fill_bio(bio);

	return ploop_write_cluster_sync(ploop, bio, cluster);
}

static int ploop_grow_relocate_cluster(struct ploop *ploop,
				       struct ploop_index_wb *piwb,
				       struct ploop_cmd *cmd)
{
	struct bio *bio = cmd->resize.bio;
	unsigned int new_dst, cluster, dst_cluster;
	bool is_locked;
	int ret = 0;

	dst_cluster = cmd->resize.dst_cluster;

	/* Relocate cluster and update index */
	cluster = ploop_find_bat_entry(ploop, dst_cluster, &is_locked);
	if (cluster == UINT_MAX || is_locked) {
		/* dst_cluster in top delta is not occupied? */
		if (!test_bit(dst_cluster, ploop->holes_bitmap) || is_locked) {
			/*
			 * No. Maybe, it's under COW. Try again later.
			 * FIXME: implement a wait list-like thing for
			 * clusters under COW and queue commands there.
			 */
			schedule_timeout(HZ/10);
			goto out;
		}
		/* Cluster is free, occupy it. Skip relocaton */
		ploop_hole_clear_bit(dst_cluster, ploop);
		goto not_occupied;
	}

	/* Redirect bios to kwork and wait inflights, which may use @cluster */
	force_defer_bio_count_inc(ploop);
	ploop_inflight_bios_ref_switch(ploop);

	/* Read full cluster sync */
	ret = ploop_read_cluster_sync(ploop, bio, dst_cluster);
	if (ret < 0)
		goto out;

	ret = ploop_prepare_reloc_index_wb(ploop, piwb, cluster,
					   &new_dst);
	if (ret < 0)
		goto out;

	/* Write cluster to new destination */
	ret = ploop_write_cluster_sync(ploop, bio, new_dst);
	if (ret) {
		ploop_reset_bat_update(piwb);
		goto out;
	}

	/* Write new index on disk */
	ploop_submit_index_wb_sync(ploop, piwb);
	ret = blk_status_to_errno(piwb->bi_status);
	ploop_reset_bat_update(piwb);
	if (ret)
		goto out;

	/* Update local BAT copy */
	write_lock_irq(&ploop->bat_rwlock);
	ploop->bat_entries[cluster] = new_dst;
	WARN_ON(!cluster_is_in_top_delta(ploop, cluster));
	write_unlock_irq(&ploop->bat_rwlock);
not_occupied:
	/*
	 * Now dst_cluster is not referenced in BAT, so increase the value
	 * for next iteration. The place we do this is significant: caller
	 * makes rollback based on this.
	 */
	cmd->resize.dst_cluster++;

	/* Zero new BAT entries on disk. */
	ret = ploop_write_zero_cluster_sync(ploop, bio, dst_cluster);
out:
	if (cluster != UINT_MAX)
		force_defer_bio_count_dec(ploop);

	return ret;
}

static int ploop_grow_update_header(struct ploop *ploop,
				    struct ploop_index_wb *piwb,
				    struct ploop_cmd *cmd)
{
	unsigned int size, first_block_off, cluster_log = ploop->cluster_log;
	struct ploop_pvd_header *hdr;
	int ret;

	/* hdr is in the same page as bat_entries[0] index */
	ret = ploop_prepare_reloc_index_wb(ploop, piwb, 0, NULL);
	if (ret)
		return ret;

	size = (PLOOP_MAP_OFFSET + cmd->resize.nr_bat_entries);
	size *= sizeof(map_index_t);
	size = DIV_ROUND_UP(size, 1 << (cluster_log + 9));
	first_block_off = size << cluster_log;

	hdr = kmap_atomic(piwb->bat_page);
	/* TODO: head and cylinders */
	hdr->m_Size = cpu_to_le32(cmd->resize.nr_bat_entries);
	hdr->m_SizeInSectors_v2 = cpu_to_le64(cmd->resize.new_size);
	hdr->m_FirstBlockOffset = cpu_to_le32(first_block_off);
	kunmap_atomic(hdr);

	ploop_submit_index_wb_sync(ploop, piwb);
	ret = blk_status_to_errno(piwb->bi_status);
	if (ret)
		goto out;

	/* Update header local copy */
	hdr = kmap_atomic(piwb->bat_page);
	write_lock_irq(&ploop->bat_rwlock);
	memcpy(ploop->hdr, hdr, sizeof(*hdr));
	write_unlock_irq(&ploop->bat_rwlock);
	kunmap_atomic(hdr);
out:
	ploop_reset_bat_update(piwb);
	return ret;
}

/*
 * Here we relocate data clusters, which may intersect with BAT area
 * of disk after resize. For user they look as already written to disk,
 * so be careful(!) and protective. Update indexes only after cluster
 * data is written to disk.
 *
 * This is called from deferred work -- the only place we alloc clusters.
 * So, nobody can reallocate clusters updated in ploop_grow_relocate_cluster().
 */
static void process_resize_cmd(struct ploop *ploop, struct ploop_index_wb *piwb,
			       struct ploop_cmd *cmd)
{
	unsigned int dst_cluster;
	int ret = 0;

	/*
	 *  Update memory arrays and hb_nr, but do not update nr_bat_entries.
	 *  This is noop except first enter to this function.
	 */
	ploop_advance_bat_and_holes(ploop, cmd);

	if (cmd->resize.dst_cluster <= cmd->resize.end_dst_cluster) {
		ret = ploop_grow_relocate_cluster(ploop, piwb, cmd);
		if (ret)
			goto out;

		/* Move one cluster per cmd to allow other requests. */
		ploop_queue_deferred_cmd(ploop, cmd);
		return;
	} else {
		/* Update header metadata */
		ret = ploop_grow_update_header(ploop, piwb, cmd);
	}

out:
	write_lock_irq(&ploop->bat_rwlock);
	if (ret) {
		/* Cleanup: mark new BAT overages as free clusters */
		dst_cluster = cmd->resize.dst_cluster - 1;

		while (dst_cluster >= cmd->resize.nr_old_bat_clu) {
			ploop_hole_set_bit(dst_cluster, ploop);
			dst_cluster--;
		}
		swap(ploop->hb_nr, cmd->resize.hb_nr);
	} else
		swap(ploop->nr_bat_entries, cmd->resize.nr_bat_entries);
	write_unlock_irq(&ploop->bat_rwlock);

	cmd->retval = ret;
	complete(&cmd->comp); /* Last touch of cmd memory */
}

struct bio *alloc_bio_with_pages(struct ploop *ploop)
{
	unsigned int cluster_log = ploop->cluster_log;
	int i, nr_pages = nr_pages_in_cluster(ploop);
	struct bio *bio;

	bio = bio_alloc(GFP_NOIO, nr_pages);
	if (!bio)
		return NULL;

	for (i = 0; i < nr_pages; i++) {
		bio->bi_io_vec[i].bv_page = alloc_page(GFP_NOIO);
		if (!bio->bi_io_vec[i].bv_page)
			goto err;
		bio->bi_io_vec[i].bv_offset = 0;
		bio->bi_io_vec[i].bv_len = PAGE_SIZE;
	}

	bio->bi_vcnt = nr_pages;
	bio->bi_iter.bi_size = 1 << (cluster_log + 9);

	return bio;
err:
	while (i-- > 0)
		put_page(bio->bi_io_vec[i].bv_page);
	bio_put(bio);
	return NULL;
}

void free_bio_with_pages(struct ploop *ploop, struct bio *bio)
{
	int i, nr_pages = bio->bi_vcnt;
	struct page *page;

	/*
	 * Not a error for this function, but the rest of code
	 * may expect this. Sanity check.
	 */
	WARN_ON_ONCE(nr_pages != nr_pages_in_cluster(ploop));

	for (i = 0; i < nr_pages; i++) {
		page = bio->bi_io_vec[i].bv_page;
		put_page(page);
	}

	bio_put(bio);
}

/* @new_size is in sectors */
static int ploop_resize(struct ploop *ploop, u64 new_size)
{
	unsigned int nr_bat_entries, nr_old_bat_clusters, nr_bat_clusters;
	unsigned int hb_nr, size, cluster_log = ploop->cluster_log;
	struct ploop_pvd_header *hdr = ploop->hdr;
	struct ploop_cmd cmd = { {0} };
	int ret = -ENOMEM;
	u64 old_size;

	if (ploop->maintaince)
		return -EBUSY;
	if (ploop_is_ro(ploop))
		return -EROFS;
	old_size = le64_to_cpu(hdr->m_SizeInSectors_v2);
	if (old_size == new_size)
		return 0;
	if (old_size > new_size) {
		DMWARN("online shrink is not supported");
		return -EINVAL;
	} else if ((new_size >> cluster_log) >= UINT_MAX - 2) {
		DMWARN("resize: too large size is requested");
		return -EINVAL;
	} else if (new_size & ((1 << cluster_log) - 1)) {
		DMWARN("resize: new_size is not aligned");
		return -EINVAL;
	}

	nr_bat_entries = (new_size >> cluster_log);

	size = nr_bat_entries * sizeof(ploop->bat_levels[0]);
	cmd.resize.bat_levels = kvzalloc(size, GFP_KERNEL);
	if (!cmd.resize.bat_levels)
		goto err;

	size = (PLOOP_MAP_OFFSET + nr_bat_entries) * sizeof(map_index_t);

	/* Memory for hdr + bat_entries */
	cmd.resize.hdr = vzalloc(size);
	if (!cmd.resize.hdr)
		goto err;

	nr_bat_clusters = DIV_ROUND_UP(size, 1 << (cluster_log + 9));
	hb_nr = nr_bat_clusters + nr_bat_entries;
	size = round_up(DIV_ROUND_UP(hb_nr, 8), sizeof(unsigned long));

	/* Currently occupied bat clusters */
	nr_old_bat_clusters = ploop_nr_bat_clusters(ploop,
						    ploop->nr_bat_entries);
	/* Memory for holes_bitmap */
	cmd.resize.holes_bitmap = kvmalloc(size, GFP_KERNEL);
	if (!cmd.resize.holes_bitmap)
		goto err;

	/* Mark all new bitmap memory as holes */
	old_size = DIV_ROUND_UP(ploop->hb_nr, 8);
	memset(cmd.resize.holes_bitmap + old_size, 0xff, size - old_size);

	cmd.resize.bio = alloc_bio_with_pages(ploop);
	if (!cmd.resize.bio)
		goto err;
	cmd.resize.bio->bi_status = 0;

	cmd.resize.cluster = UINT_MAX;
	cmd.resize.dst_cluster = nr_old_bat_clusters;
	cmd.resize.end_dst_cluster = nr_bat_clusters - 1;
	cmd.resize.nr_old_bat_clu = nr_old_bat_clusters;
	cmd.resize.nr_bat_entries = nr_bat_entries;
	cmd.resize.hb_nr = hb_nr;
	cmd.resize.new_size = new_size;
	cmd.retval = 0;
	cmd.type = PLOOP_CMD_RESIZE;
	cmd.ploop = ploop;

	init_completion(&cmd.comp);
	ploop_queue_deferred_cmd(ploop, &cmd);
	wait_for_completion(&cmd.comp);

	ret = cmd.retval;
err:
	if (cmd.resize.bio)
		free_bio_with_pages(ploop, cmd.resize.bio);
	kvfree(cmd.resize.bat_levels);
	kvfree(cmd.resize.holes_bitmap);
	vfree(cmd.resize.hdr);
	return ret;
}

/* FIXME: this must not be called on running device */
static void process_add_delta_cmd(struct ploop *ploop, struct ploop_cmd *cmd)
{
	map_index_t *bat_entries, *delta_bat_entries;
	unsigned int i, level, dst_cluster;
	u8 *bat_levels;

	if (unlikely(ploop->force_link_inflight_bios)) {
		cmd->retval = -EBUSY;
		pr_err("ploop: adding delta on running device\n");
		goto out;
	}

	level = ploop->nr_deltas;
	bat_entries = ploop->bat_entries;
	bat_levels = ploop->bat_levels;
	delta_bat_entries = (map_index_t *)cmd->add_delta.hdr + PLOOP_MAP_OFFSET;

	write_lock_irq(&ploop->bat_rwlock);

	/* FIXME: Stop on old delta's nr_bat_entries */
	for (i = 0; i < ploop->nr_bat_entries; i++) {
		if (cluster_is_in_top_delta(ploop, i))
			continue;
		if (!cmd->add_delta.is_raw)
			dst_cluster = delta_bat_entries[i];
		else
			dst_cluster = i < cmd->add_delta.raw_clusters ? i : BAT_ENTRY_NONE;
		if (dst_cluster == BAT_ENTRY_NONE)
			continue;
		/*
		 * Prefer last added delta, since the order is:
		 * 1)add top device
		 * 2)add oldest delta
		 * ...
		 * n)add newest delta
		 * Keep in mind, top device is current image, and
		 * it is added first in contrary the "age" order.
		 */
		bat_levels[i] = level;
		bat_entries[i] = dst_cluster;
	}

	swap(ploop->deltas, cmd->add_delta.deltas);
	ploop->nr_deltas++;
	write_unlock_irq(&ploop->bat_rwlock);
	get_file(ploop->deltas[level]);
	cmd->retval = 0;
out:
	complete(&cmd->comp); /* Last touch of cmd memory */
}

static int ploop_check_raw_delta(struct ploop *ploop, struct file *file,
				 struct ploop_cmd *cmd)
{
	loff_t loff = i_size_read(file->f_mapping->host);
	unsigned int cluster_log = ploop->cluster_log;

	if (loff & ((1 << (cluster_log + SECTOR_SHIFT)) - 1))
		return -EPROTO;
	cmd->add_delta.raw_clusters = loff >> (cluster_log + SECTOR_SHIFT);
	return 0;
}

/*
 * @fd refers to a new delta, which is placed right before top_delta.
 * So, userspace has to populate deltas stack from oldest to newest.
 */
int ploop_add_delta(struct ploop *ploop, const char *arg)
{
	unsigned int level = ploop->nr_deltas;
	struct ploop_cmd cmd = { {0} };
	struct file **deltas;
	bool is_raw = false;
	unsigned int size;
	struct file *file;
	int fd, ret;

	if (ploop->maintaince)
		return -EBUSY;
	if (strncmp(arg, "raw@", 4) == 0) {
		is_raw = true;
		arg += 4;
	}
	if (level == BAT_LEVEL_TOP || (is_raw && level))
		return -EMFILE;
	if (kstrtos32(arg, 10, &fd) < 0)
		return -EINVAL;

	file = fget(fd);
	if (!file)
		return -ENOENT;
	ret = -EBADF;
	if (!(file->f_mode & FMODE_READ))
		goto out;

	ret = -ENOMEM;
	deltas = kcalloc(level + 1, sizeof(*file), GFP_KERNEL);
	if (!deltas)
		goto out;
	size = level * sizeof(*file);
	memcpy(deltas, ploop->deltas, size);
	deltas[level] = file;
	/*
	 * BAT update in general is driven by the kwork
	 * (see comment in process_one_deferred_bio()),
	 * so we delegate the cmd to it.
	 */
	cmd.add_delta.deltas = deltas;
	cmd.add_delta.is_raw = is_raw;
	cmd.type = PLOOP_CMD_ADD_DELTA;
	cmd.ploop = ploop;

	if (is_raw)
		ret = ploop_check_raw_delta(ploop, file, &cmd);
	else
		ret = ploop_read_delta_metadata(ploop, file,
						&cmd.add_delta.hdr);
	if (ret)
		goto out;

	init_completion(&cmd.comp);
	ploop_queue_deferred_cmd(ploop, &cmd);
	wait_for_completion(&cmd.comp);
	ret = cmd.retval;
out:
	vfree(cmd.add_delta.hdr);
	kfree(cmd.add_delta.deltas);
	fput(file);
	return ret;
}
static void ploop_queue_deferred_cmd_wrapper(struct ploop *ploop,
					     int ret, void *data)
{
	struct ploop_cmd *cmd = data;

	if (ret) {
		/* kwork will see this at next time it is on cpu */
		WRITE_ONCE(cmd->retval, ret);
	}
	atomic_inc(&cmd->merge.nr_available);
	ploop_queue_deferred_cmd(cmd->ploop, cmd);
}

/* Find mergeable cluster and return it in cmd->merge.cluster */
static bool iter_delta_clusters(struct ploop *ploop, struct ploop_cmd *cmd)
{
	unsigned int *cluster = &cmd->merge.cluster;
	unsigned int level;
	bool skip;

	BUG_ON(cmd->type != PLOOP_CMD_MERGE_SNAPSHOT);

	for (; *cluster < ploop->nr_bat_entries; ++*cluster) {
		/*
		 * Check *cluster is provided by the merged delta.
		 * We are in kwork, so bat_rwlock is not needed
		 * (see comment in process_one_deferred_bio()).
		 */
		level = ploop->bat_levels[*cluster];
		if (ploop->bat_entries[*cluster] == BAT_ENTRY_NONE ||
		    level != ploop->nr_deltas - 1)
			continue;

		spin_lock_irq(&ploop->deferred_lock);
		skip = find_lk_of_cluster(ploop, *cluster);
		spin_unlock_irq(&ploop->deferred_lock);
		if (skip) {
			/*
			 * Cluster is locked (maybe, under COW).
			 * Skip it and try to repeat later.
			 */
			cmd->merge.do_repeat = true;
			continue;
		}

		return true;
	}

	return false;
}

static void process_merge_latest_snapshot_cmd(struct ploop *ploop,
					      struct ploop_cmd *cmd)
{
	unsigned int *cluster = &cmd->merge.cluster;
	unsigned int level, dst_cluster;
	struct file *file;

	if (cmd->retval)
		goto out;

	while (iter_delta_clusters(ploop, cmd)) {
		/*
		 * We are in kwork, so bat_rwlock is not needed
		 * (we can't race with changing BAT, since cmds
		 *  are processed before bios and piwb is sync).
		 */
		dst_cluster = ploop->bat_entries[*cluster];
		level = ploop->bat_levels[*cluster];

		/* Check we can submit one more cow in parallel */
		if (!atomic_add_unless(&cmd->merge.nr_available, -1, 0))
			return;

		if (submit_cluster_cow(ploop, level, *cluster, dst_cluster,
				    ploop_queue_deferred_cmd_wrapper, cmd)) {
			atomic_inc(&cmd->merge.nr_available);
			cmd->retval = -ENOMEM;
			goto out;
		}

		++*cluster;
	}
out:
	if (atomic_read(&cmd->merge.nr_available) != NR_MERGE_BIOS) {
		/* Wait till last COW queues us */
		return;
	}

	if (cmd->retval == 0 && !cmd->merge.do_repeat) {
		/* Delta merged. Release delta's file */
		write_lock_irq(&ploop->bat_rwlock);
		file = ploop->deltas[--ploop->nr_deltas];
		write_unlock_irq(&ploop->bat_rwlock);
		ploop_inflight_bios_ref_switch(ploop);
		fput(file);
	}
	complete(&cmd->comp); /* Last touch of cmd memory */
}

static int ploop_merge_latest_snapshot(struct ploop *ploop)
{
	struct ploop_cmd cmd;
	int ret;

	if (ploop->maintaince)
		return -EBUSY;
	if (ploop_is_ro(ploop))
		return -EROFS;
	if (!ploop->nr_deltas)
		return -ENOENT;
again:
	memset(&cmd, 0, sizeof(cmd));
	cmd.type = PLOOP_CMD_MERGE_SNAPSHOT;
	cmd.ploop = ploop;
	atomic_set(&cmd.merge.nr_available, NR_MERGE_BIOS);

	init_completion(&cmd.comp);
	ploop_queue_deferred_cmd(ploop, &cmd);
	ret = wait_for_completion_interruptible(&cmd.comp);
	if (ret) {
		/*
		 * process_merge_latest_snapshot_cmd() will see this
		 * later or earlier. Take a lock if you want earlier.
		 */
		WRITE_ONCE(cmd.retval, -EINTR);
		wait_for_completion(&cmd.comp);
	}

	if (cmd.retval == 0 && cmd.merge.do_repeat)
		goto again;

	return cmd.retval;
}

static void process_notify_delta_merged(struct ploop *ploop,
					struct ploop_cmd *cmd)
{
	unsigned int i, *bat_entries, *delta_bat_entries;
	void *hdr = cmd->notify_delta_merged.hdr;
	u8 level = cmd->notify_delta_merged.level;
	struct file *file;
	u8 *bat_levels;

	bat_entries = ploop->bat_entries;
	bat_levels = ploop->bat_levels;
	delta_bat_entries = (map_index_t *)hdr + PLOOP_MAP_OFFSET;

	write_lock_irq(&ploop->bat_rwlock);
	for (i = 0; i < ploop->nr_bat_entries; i++) {
		if (cluster_is_in_top_delta(ploop, i) ||
		    delta_bat_entries[i] == BAT_ENTRY_NONE ||
		    bat_levels[i] < level) {
			continue;
		}

		/* deltas above @level become renumbered */
		if (bat_levels[i] > level) {
			bat_levels[i]--;
			continue;
		}

		/*
		 * clusters from deltas of @level become pointing to next delta
		 * (which became renumbered) or prev delta (if !@forward).
		 */
		bat_entries[i] = delta_bat_entries[i];
		WARN_ON(bat_entries[i] == BAT_ENTRY_NONE);
		if (!cmd->notify_delta_merged.forward)
			bat_levels[i]--;
	}

	file = ploop->deltas[level];
	/* Renumber deltas above @level */
	for (i = level + 1; i < ploop->nr_deltas; i++)
		ploop->deltas[i - 1] = ploop->deltas[i];
	ploop->deltas[--ploop->nr_deltas] = NULL;
	write_unlock_irq(&ploop->bat_rwlock);

	ploop_inflight_bios_ref_switch(ploop);
	fput(file);

	cmd->retval = 0;
	complete(&cmd->comp); /* Last touch of cmd memory */
}

static void process_update_delta_index(struct ploop *ploop,
				       struct ploop_cmd *cmd)
{
	const char *map = cmd->update_delta_index.map;
	u8 level = cmd->update_delta_index.level;
	unsigned int cluster, dst_cluster, n;
	int ret = -EINVAL;

	write_lock_irq(&ploop->bat_rwlock);
	/* Check all */
	while (sscanf(map, "%u:%u;%n", &cluster, &dst_cluster, &n) == 2) {
		if (ploop->bat_entries[cluster] == BAT_ENTRY_NONE)
			break;
		if (cluster >= ploop->nr_bat_entries)
			break;
		map += n;
	}
	if (map[0] != '\0')
		goto unlock;
	/* Commit all */
	map = cmd->update_delta_index.map;
	while (sscanf(map, "%u:%u;%n", &cluster, &dst_cluster, &n) == 2) {
		if (ploop->bat_levels[cluster] == level)
			ploop->bat_entries[cluster] = dst_cluster;
		map += n;
	}
	ret = 0;
unlock:
	write_unlock_irq(&ploop->bat_rwlock);
	if (!ret)
		ploop_inflight_bios_ref_switch(ploop);

	cmd->retval = ret;
	complete(&cmd->comp); /* Last touch of cmd memory */
}

static int ploop_delta_clusters_merged(struct ploop *ploop, u8 level,
				       bool forward)
{
	struct ploop_cmd cmd = { {0} };
	void *d_hdr = NULL;
	struct file *file;
	int ret;

	/* Reread BAT of deltas[@level + 1] (or [@level - 1]) */
	file = ploop->deltas[level + forward ? 1 : -1];

	ret = ploop_read_delta_metadata(ploop, file, &d_hdr);
	if (ret)
		goto out;

	cmd.notify_delta_merged.level = level;
	cmd.notify_delta_merged.hdr = d_hdr;
	cmd.notify_delta_merged.forward = forward;
	cmd.type = PLOOP_CMD_NOTIFY_DELTA_MERGED;
	cmd.ploop = ploop;

	init_completion(&cmd.comp);
	ploop_queue_deferred_cmd(ploop, &cmd);
	wait_for_completion(&cmd.comp);
	ret = cmd.retval;
out:
	vfree(d_hdr);
	return ret;
}

static int ploop_notify_merged(struct ploop *ploop, u8 level, bool forward)
{
	if (ploop->maintaince)
		return -EBUSY;
	if (level >= ploop->nr_deltas)
		return -ENOENT;
	if (level == 0 && !forward)
		return -EINVAL;
	if (level == ploop->nr_deltas - 1 && forward)
		return -EINVAL;
	/*
	 * Userspace notifies us, it has copied clusters of
	 * ploop->deltas[@level] to ploop->deltas[@level + 1]
	 * (deltas[@level] to deltas[@level - 1] if !@forward).
	 * Now we want to update our bat_entries/levels arrays,
	 * where ploop->deltas[@level] is used currently, to use
	 * @level + 1 instead. Also we want to put @level's file,
	 * and renumerate deltas.
	 */
	return ploop_delta_clusters_merged(ploop, level, forward);
}

static int ploop_get_delta_name_cmd(struct ploop *ploop, u8 level,
				char *result, unsigned int maxlen)
{
	struct file *file;
	int len, ret = 1;
	char *p;

	if (level >= ploop->nr_deltas) {
		result[0] = '\0';
		goto out;
	}

	/*
	 * Nobody can change deltas in parallel, since
	 * another cmds are prohibited, but do this
	 * for uniformity.
	 */
	read_lock_irq(&ploop->bat_rwlock);
	file = get_file(ploop->deltas[level]);
	read_unlock_irq(&ploop->bat_rwlock);

	p = file_path(file, result, maxlen);
	if (p == ERR_PTR(-ENAMETOOLONG)) {
		/* Notify target_message(), there is not enough space */
		memset(result, 'x', maxlen - 1);
		result[maxlen - 1] = 0;
	} else if (IS_ERR_OR_NULL(p)) {
		ret = PTR_ERR(p);
	} else {
		len = strlen(p);
		memmove(result, p, len);
		result[len] = '\n';
		result[len + 1] = '\0';
	}

	fput(file);
out:
	return ret;
}

static int ploop_update_delta_index(struct ploop *ploop, unsigned int level,
				    const char *map)
{
	struct ploop_cmd cmd = { {0} };

	if (ploop->maintaince)
		return -EBUSY;
	if (level >= ploop->nr_deltas)
		return -ENOENT;

	cmd.update_delta_index.level = level;
	cmd.update_delta_index.map = map;
	cmd.type = PLOOP_CMD_UPDATE_DELTA_INDEX;
	cmd.ploop = ploop;

	init_completion(&cmd.comp);
	ploop_queue_deferred_cmd(ploop, &cmd);
	wait_for_completion(&cmd.comp);
	return cmd.retval;
}

static void process_switch_top_delta(struct ploop *ploop, struct ploop_cmd *cmd)
{
	unsigned int i, size, bat_clusters, level = ploop->nr_deltas;

	force_defer_bio_count_inc(ploop);
	ploop_inflight_bios_ref_switch(ploop);

	/* If you add more two-stages-actions, you must cancel them here too */
	cancel_discard_bios(ploop);
	restart_delta_cow(ploop);

	write_lock_irq(&ploop->bat_rwlock);
	swap(ploop->origin_dev, cmd->switch_top_delta.origin_dev);
	swap(ploop->deltas, cmd->switch_top_delta.deltas);
	for (i = 0; i < ploop->nr_bat_entries; i++)
		if (ploop->bat_levels[i] == BAT_LEVEL_TOP)
			ploop->bat_levels[i] = level;

	/* Header and BAT-occupied clusters at start of file */
	size = (PLOOP_MAP_OFFSET + ploop->nr_bat_entries) * sizeof(map_index_t);
	bat_clusters = DIV_ROUND_UP(size, 1 << (ploop->cluster_log + 9));
	for (i = 0; i < ploop->hb_nr; i++) {
		if (i < bat_clusters)
			clear_bit(i, ploop->holes_bitmap);
		else
			set_bit(i, ploop->holes_bitmap);
	}

	ploop->nr_deltas++;
	write_unlock_irq(&ploop->bat_rwlock);
	force_defer_bio_count_dec(ploop);

	cmd->retval = 0;
	complete(&cmd->comp); /* Last touch of cmd memory */
}

/* Switch top delta to new device after userspace has created snapshot */
static int ploop_switch_top_delta(struct ploop *ploop, int new_ro_fd,
				  char *new_dev)
{
	struct dm_target *ti = ploop->ti;
	struct ploop_cmd cmd = { {0} };
	struct file *file;
	unsigned int size;
	int ret;

	cmd.type = PLOOP_CMD_SWITCH_TOP_DELTA;
	cmd.ploop = ploop;

	if (ploop->maintaince)
		return -EBUSY;
	if (ploop->nr_deltas == BAT_LEVEL_TOP)
		return -EMFILE;
	if (!(file = fget(new_ro_fd)))
		return -EBADF;
	ret = dm_get_device(ti, new_dev, dm_table_get_mode(ti->table),
			    &cmd.switch_top_delta.origin_dev);
	if (ret)
		goto fput;
	ret = -ENOMEM;
	size = (ploop->nr_deltas + 1) * sizeof(struct file *);
	cmd.switch_top_delta.deltas = kmalloc(size, GFP_NOIO);
	if (!cmd.switch_top_delta.deltas)
		goto put_dev;
	size -= sizeof(struct file *);
	memcpy(cmd.switch_top_delta.deltas, ploop->deltas, size);
	cmd.switch_top_delta.deltas[ploop->nr_deltas] = file;

	init_completion(&cmd.comp);
	ploop_queue_deferred_cmd(ploop, &cmd);
	wait_for_completion(&cmd.comp);
	ret = cmd.retval;
	kfree(cmd.switch_top_delta.deltas);
put_dev:
	dm_put_device(ploop->ti, cmd.switch_top_delta.origin_dev);
fput:
	if (ret)
		fput(file);
	return ret;
}

static void process_flip_upper_deltas(struct ploop *ploop, struct ploop_cmd *cmd)
{
	unsigned int i, size, bat_clusters, hb_nr = ploop->hb_nr;
	void *holes_bitmap = ploop->holes_bitmap;
	u8 level = ploop->nr_deltas - 1;

	size = (PLOOP_MAP_OFFSET + ploop->nr_bat_entries) * sizeof(map_index_t);
        bat_clusters = DIV_ROUND_UP(size, 1 << (ploop->cluster_log + 9));

	write_lock_irq(&ploop->bat_rwlock);
	/* Prepare holes_bitmap */
	memset(holes_bitmap, 0xff, hb_nr/8);
	for (i = (hb_nr & ~0x7); i < hb_nr; i++)
		set_bit(i, holes_bitmap);
	for (i = 0; i < bat_clusters; i++)
		clear_bit(i, holes_bitmap);

	/* Flip bat entries */
	for (i = 0; i < ploop->nr_bat_entries; i++) {
		if (ploop->bat_entries[i] == BAT_ENTRY_NONE)
			continue;
		if (ploop->bat_levels[i] == level) {
			ploop->bat_levels[i] = BAT_LEVEL_TOP;
			clear_bit(ploop->bat_entries[i], holes_bitmap);
		} else if (ploop->bat_levels[i] == BAT_LEVEL_TOP) {
			ploop->bat_levels[i] = level;
		}
	}
	swap(ploop->origin_dev, cmd->flip_upper_deltas.origin_dev);
	swap(ploop->deltas[level], cmd->flip_upper_deltas.file);
	write_unlock_irq(&ploop->bat_rwlock);
	/* Device is suspended, but anyway... */
	ploop_inflight_bios_ref_switch(ploop);

	cmd->retval = 0;
	complete(&cmd->comp); /* Last touch of cmd memory */
}

static void process_tracking_start(struct ploop *ploop, struct ploop_cmd *cmd)
{
	unsigned int i, dst_cluster, tb_nr = cmd->tracking_start.tb_nr;
	void *tracking_bitmap = cmd->tracking_start.tracking_bitmap;
	int ret = 0;

	write_lock_irq(&ploop->bat_rwlock);
	ploop->tracking_bitmap = tracking_bitmap;
	ploop->tb_nr = tb_nr;
	write_unlock_irq(&ploop->bat_rwlock);

	/*
	 * Here we care about ploop_map() sees ploop->tracking_bitmap,
	 * since the rest of submitting are made from *this* kwork.
	 */
	ploop_inflight_bios_ref_switch(ploop);

	write_lock_irq(&ploop->bat_rwlock);
	for_each_clear_bit(i, ploop->holes_bitmap, ploop->hb_nr)
		set_bit(i, tracking_bitmap);
	for (i = 0; i < ploop->nr_bat_entries; i++) {
		if (!cluster_is_in_top_delta(ploop, i))
			continue;
		dst_cluster = ploop->bat_entries[i];
		if (WARN_ON(dst_cluster >= tb_nr)) {
			ret = -EIO;
			goto unlock;
		}
		set_bit(dst_cluster, tracking_bitmap);
	}
unlock:
	write_unlock_irq(&ploop->bat_rwlock);

	cmd->retval = ret;
	complete(&cmd->comp); /* Last touch of cmd memory */
}

static int tracking_get_next(struct ploop *ploop, char *result,
			     unsigned int maxlen)
{
	unsigned int i, sz = 0, tb_nr = ploop->tb_nr, prev = ploop->tb_cursor;
	void *tracking_bitmap = ploop->tracking_bitmap;
	int ret = -EAGAIN;

	if (WARN_ON_ONCE(prev > tb_nr - 1))
		prev = 0;

	write_lock_irq(&ploop->bat_rwlock);
	i = find_next_bit(tracking_bitmap, tb_nr, prev + 1);
	if (i < tb_nr)
		goto found;
	i = find_first_bit(tracking_bitmap, prev + 1);
	if (i >= prev + 1)
		goto unlock;
found:
	ret = (DMEMIT("%u\n", i)) ? 1 : 0;
	if (ret)
		clear_bit(i, tracking_bitmap);
unlock:
	write_unlock_irq(&ploop->bat_rwlock);
	if (ret > 0)
		ploop->tb_cursor = i;
	return ret;
}

static int ploop_tracking_cmd(struct ploop *ploop, const char *suffix,
			      char *result, unsigned int maxlen)
{
	struct ploop_cmd cmd = { {0} };
	void *tracking_bitmap = NULL;
	unsigned int i, tb_nr, size;

	if (ploop_is_ro(ploop))
		return -EROFS;

	if (!strcmp(suffix, "get_next")) {
		if (!ploop->tracking_bitmap)
			return -ENOENT;
		return tracking_get_next(ploop, result, maxlen);
	}

	if (!strcmp(suffix, "start")) {
		if (ploop->tracking_bitmap)
			return -EEXIST;
		if (ploop->maintaince)
			return -EBUSY;
		tb_nr = ploop->hb_nr;
		read_lock_irq(&ploop->bat_rwlock);
		for (i = 0; i < ploop->nr_bat_entries; i++)
			if (cluster_is_in_top_delta(ploop, i) &&
			    ploop->bat_entries[i] >= tb_nr)
				tb_nr = ploop->bat_entries[i] + 1;
		read_unlock_irq(&ploop->bat_rwlock);
		/*
		 * After unlock new entries above tb_nr can't
		 * occur, since we always alloc clusters from
		 * holes_bitmap (and they nr < hb_nr).
		 */
		size = DIV_ROUND_UP(tb_nr, 8 * sizeof(unsigned long));
		size *= sizeof(unsigned long);
		tracking_bitmap = kvzalloc(size, GFP_KERNEL);
		if (!tracking_bitmap)
			return -ENOMEM;
		ploop->tb_cursor = tb_nr - 1;

		cmd.type = PLOOP_CMD_TRACKING_START;
		cmd.ploop = ploop;
		cmd.tracking_start.tracking_bitmap = tracking_bitmap;
		cmd.tracking_start.tb_nr = tb_nr;

		init_completion(&cmd.comp);
		ploop_queue_deferred_cmd(ploop, &cmd);
		wait_for_completion(&cmd.comp);
		ploop->maintaince = true;
	} else if (!strcmp(suffix, "stop")) {
		if (!ploop->tracking_bitmap)
			return -ENOENT;
		write_lock_irq(&ploop->bat_rwlock);
		kvfree(ploop->tracking_bitmap);
		ploop->tracking_bitmap = NULL;
		write_unlock_irq(&ploop->bat_rwlock);
		ploop->maintaince = false;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int ploop_set_noresume(struct ploop *ploop, char *mode)
{
	bool noresume;

	if (!strcmp(mode, "1"))
		noresume = true;
	else if (!strcmp(mode, "0"))
		noresume = false;
	else
		return -EINVAL;

	if (noresume == ploop->noresume)
		return -EBUSY;

	ploop->noresume = noresume;
	return 0;
}

static int ploop_flip_upper_deltas(struct ploop *ploop, char *new_dev,
				   char *new_ro_fd)
{
	struct dm_target *ti = ploop->ti;
	struct ploop_cmd cmd = { {0} };
	int new_fd, ret;

	cmd.type = PLOOP_CMD_FLIP_UPPER_DELTAS;
	cmd.ploop = ploop;

	/* FIXME: prohibit flip on raw delta */
	if (!dm_suspended(ti) || !ploop->noresume || ploop->maintaince)
		return -EBUSY;
	if (ploop_is_ro(ploop))
		return -EROFS;
	if (!ploop->nr_deltas)
		return -ENOENT;
	if (kstrtou32(new_ro_fd, 10, &new_fd) < 0 ||
	    !(cmd.flip_upper_deltas.file = fget(new_fd)))
		return -EBADF;
	ret = dm_get_device(ti, new_dev, dm_table_get_mode(ti->table),
			    &cmd.flip_upper_deltas.origin_dev);
	if (ret)
		goto fput;

	init_completion(&cmd.comp);
	ploop_queue_deferred_cmd(ploop, &cmd);
	wait_for_completion(&cmd.comp);
	ret = cmd.retval;
	dm_put_device(ploop->ti, cmd.flip_upper_deltas.origin_dev);
fput:
	fput(cmd.flip_upper_deltas.file);
	return ret;
}

static void process_set_push_backup(struct ploop *ploop, struct ploop_cmd *cmd)
{
	struct push_backup *pb = cmd->set_push_backup.pb;

	if (!pb)
		cleanup_backup(ploop);

	spin_lock_irq(&ploop->pb_lock);
	/* Take bat_rwlock to make pb visible in ploop_map() */
	write_lock(&ploop->bat_rwlock);
	swap(ploop->pb, pb);
	write_unlock(&ploop->bat_rwlock);
	spin_unlock_irq(&ploop->pb_lock);
	cmd->retval = 0;
	complete(&cmd->comp); /* Last touch of cmd memory */

	if (pb)
		ploop_free_pb(pb);
}

static struct push_backup *ploop_alloc_pb(struct ploop *ploop, char *uuid)
{
	struct push_backup *pb;
	unsigned int size;
	void *map;

	pb = kzalloc(sizeof(*pb), GFP_KERNEL);
	if (!pb)
		return NULL;
	snprintf(pb->uuid, sizeof(pb->uuid), "%s", uuid);
	init_waitqueue_head(&pb->wq);
	INIT_LIST_HEAD(&pb->pending);
	pb->rb_root = RB_ROOT;

	size = DIV_ROUND_UP(ploop->nr_bat_entries, 8);
	size = round_up(size, sizeof(unsigned long));
	map = kvzalloc(size, GFP_KERNEL);
	if (!map)
		goto out_pb;

	pb->ppb_map = map;
	return pb;
out_pb:
	kfree(pb);
	return NULL;
}

void ploop_free_pb(struct push_backup *pb)
{
	WARN_ON(!RB_EMPTY_ROOT(&pb->rb_root));
	kvfree(pb->ppb_map);
	kfree(pb);
}

static void ploop_pb_timer(struct timer_list *timer)
{
	struct push_backup *pb = from_timer(pb, timer, deadline_timer);
	u64 deadline, now = get_jiffies_64();
	struct ploop *ploop = pb->ploop;
	unsigned long flags;

	spin_lock_irqsave(&ploop->pb_lock, flags);
	deadline = pb->deadline_jiffies;
	spin_unlock_irqrestore(&ploop->pb_lock, flags);

	if (unlikely(time_before64(now, deadline)))
		mod_timer(timer, deadline - now + 1);
	else
		queue_work(ploop->wq, &ploop->worker);
}

static void ploop_setup_pb(struct ploop *ploop, struct push_backup *pb)
{
	unsigned int i, nr_bat_entries = ploop->nr_bat_entries;

	/* Full backup */
	memset(pb->ppb_map, 0xff, nr_bat_entries / 8);
	for (i = round_down(nr_bat_entries, 8); i < nr_bat_entries; i++)
		set_bit(i, pb->ppb_map);

	pb->deadline_jiffies = S64_MAX;
	timer_setup(&pb->deadline_timer, ploop_pb_timer, 0);

	pb->ploop = ploop;
	pb->alive = true;
}

static int ploop_push_backup_start(struct ploop *ploop, char *uuid,
				   void __user *mask)
{
	struct ploop_cmd cmd = { {0} };
	struct push_backup *pb;
	char *p = uuid;

	cmd.type = PLOOP_CMD_SET_PUSH_BACKUP;
	cmd.ploop = ploop;

	if (mask)
		return -ENOPROTOOPT; /* TODO */

	if (ploop->pb)
		return -EEXIST;
	/*
	 * There is no a problem in case of not suspended for the device.
	 * But this means userspace collects wrong backup. Warn it here.
	 * Since the device is suspended, we do not care about inflight bios.
	 */
	if (!dm_suspended(ploop->ti) || ploop->maintaince)
		return -EBUSY;
	/* Check UUID */
	while (*p) {
		if (!isxdigit(*p))
			return -EINVAL;
		p++;
	}
	if (p != uuid + sizeof(pb->uuid) - 1)
		return -EINVAL;
	pb = ploop_alloc_pb(ploop, uuid);
	if (!pb)
		return -ENOMEM;
	ploop_setup_pb(ploop, pb);

	/* Assign pb in work, to make it visible w/o locks (in work) */
	cmd.set_push_backup.pb = pb;
	init_completion(&cmd.comp);
	ploop_queue_deferred_cmd(ploop, &cmd);
	wait_for_completion(&cmd.comp);
	ploop->maintaince = true;
	return 0;
}

static int ploop_push_backup_stop(struct ploop *ploop, char *uuid,
		   int uretval, char *result, unsigned int maxlen)
{
	struct ploop_cmd cmd = { {0} };
	unsigned int sz = 0;

	cmd.type = PLOOP_CMD_SET_PUSH_BACKUP;
	cmd.ploop = ploop;

	if (!ploop->pb)
		return -EBADF;
	if (strcmp(ploop->pb->uuid, uuid))
		return -EINVAL;

	WARN_ON(!ploop->maintaince);

	/* Assign pb in work, to make it visible w/o locks (in work) */
	init_completion(&cmd.comp);
	ploop_queue_deferred_cmd(ploop, &cmd);
	wait_for_completion(&cmd.comp);
	ploop->maintaince = false;
	DMEMIT("0");
	return 1;
}

static int ploop_push_backup_get_uuid(struct ploop *ploop, char *result,
				      unsigned int maxlen)
{
	struct push_backup *pb = ploop->pb;
	unsigned int sz = 0;

	if (pb)
		DMEMIT("%s", pb->uuid);
	else
		result[0] = '\0';
	return 1;
}

static int ploop_push_backup_read(struct ploop *ploop, char *uuid,
				char *result, unsigned int maxlen)
{
	struct dm_ploop_endio_hook *h, *orig_h;
	struct push_backup *pb = ploop->pb;
	unsigned int left, right, sz = 0;
	struct rb_node *node;
	int ret = 1;

	if (!pb)
		return -EBADF;
	if (strcmp(uuid, pb->uuid))
		return -EINVAL;
	if (!pb->alive)
		return -ESTALE;
again:
	if (wait_event_interruptible(pb->wq, !list_empty_careful(&pb->pending)))
		return -EINTR;

	spin_lock_irq(&ploop->pb_lock);
	h = orig_h = list_first_entry_or_null(&pb->pending, typeof(*h), list);
	if (unlikely(!h)) {
		spin_unlock_irq(&ploop->pb_lock);
		goto again;
	}
	list_del_init(&h->list);

	left = right = h->cluster;
	while ((node = rb_prev(&h->node)) != NULL) {
		h = rb_entry(node, struct dm_ploop_endio_hook, node);
		if (h->cluster + 1 != left || list_empty(&h->list))
			break;
		list_del_init(&h->list);
		left = h->cluster;
	}

	h = orig_h;
	while ((node = rb_next(&h->node)) != NULL) {
		h = rb_entry(node, struct dm_ploop_endio_hook, node);
		if (h->cluster - 1 != right || list_empty(&h->list))
			break;
		list_del_init(&h->list);
		right = h->cluster;
	}

	DMEMIT("%u:%u", left, right - left + 1);
	spin_unlock_irq(&ploop->pb_lock);
	return ret;
}

static int ploop_push_backup_write(struct ploop *ploop, char *uuid,
			     unsigned int cluster, unsigned int nr)
{
	unsigned int i, nr_bat_entries = ploop->nr_bat_entries;
	struct bio_list bio_list = BIO_EMPTY_LIST;
	struct push_backup *pb = ploop->pb;
	struct dm_ploop_endio_hook *h;
	bool has_more = false;

	if (!pb)
		return -EBADF;
	if (strcmp(uuid, pb->uuid) || !nr)
		return -EINVAL;
	if (cluster >= nr_bat_entries || nr > nr_bat_entries - cluster)
		return -E2BIG;
	if (!pb->alive)
		return -ESTALE;

	spin_lock_irq(&ploop->pb_lock);
	for (i = cluster; i < cluster + nr; i++)
		clear_bit(i, pb->ppb_map);
	for (i = 0; i < nr; i++) {
		h = find_endio_hook_range(ploop, &pb->rb_root, cluster,
					  cluster + nr - 1);
		if (!h)
			break;
		unlink_postponed_backup_endio(ploop, &bio_list, h);
	}

	has_more = !RB_EMPTY_ROOT(&pb->rb_root);
	if (has_more)
		pb->deadline_jiffies = get_jiffies_64() + BACKUP_DEADLINE * HZ;
	else
		pb->deadline_jiffies = S64_MAX;
	spin_unlock_irq(&ploop->pb_lock);

	if (!bio_list_empty(&bio_list)) {
		defer_bio_list(ploop, &bio_list);
		if (has_more)
			mod_timer(&pb->deadline_timer, BACKUP_DEADLINE * HZ + 1);
	}

	return 0;
}

/* Handle user commands requested via "message" interface */
void process_deferred_cmd(struct ploop *ploop, struct ploop_index_wb *piwb)
	__releases(&ploop->deferred_lock)
	__acquires(&ploop->deferred_lock)
{
	struct ploop_cmd *cmd = ploop->deferred_cmd;

	if (likely(!cmd))
		return;

	ploop->deferred_cmd = NULL;
	spin_unlock_irq(&ploop->deferred_lock);

	/* There must not be a pending index wb */
	WARN_ON(piwb->page_nr != PAGE_NR_NONE);

	if (cmd->type == PLOOP_CMD_RESIZE) {
		process_resize_cmd(ploop, piwb, cmd);
	} else if (cmd->type == PLOOP_CMD_ADD_DELTA) {
		process_add_delta_cmd(ploop, cmd);
	} else if (cmd->type == PLOOP_CMD_MERGE_SNAPSHOT) {
		process_merge_latest_snapshot_cmd(ploop, cmd);
	} else if (cmd->type == PLOOP_CMD_NOTIFY_DELTA_MERGED) {
		process_notify_delta_merged(ploop, cmd);
	} else if (cmd->type == PLOOP_CMD_SWITCH_TOP_DELTA) {
		process_switch_top_delta(ploop, cmd);
	} else if (cmd->type == PLOOP_CMD_UPDATE_DELTA_INDEX) {
		process_update_delta_index(ploop, cmd);
	} else if (cmd->type == PLOOP_CMD_TRACKING_START) {
		process_tracking_start(ploop, cmd);
	} else if (cmd->type == PLOOP_CMD_FLIP_UPPER_DELTAS) {
		process_flip_upper_deltas(ploop, cmd);
	} else if (cmd->type == PLOOP_CMD_SET_PUSH_BACKUP) {
		process_set_push_backup(ploop, cmd);
	} else {
		cmd->retval = -EINVAL;
		complete(&cmd->comp);
	}
	spin_lock_irq(&ploop->deferred_lock);
}

static bool msg_wants_down_read(const char *cmd)
{
	if (!strcmp(cmd, "get_delta_name") ||
	    !strcmp(cmd, "push_backup_get_uuid") ||
	    !strcmp(cmd, "push_backup_read") ||
	    !strcmp(cmd, "push_backup_write"))
		return true;

	return false;
}

int ploop_message(struct dm_target *ti, unsigned int argc, char **argv,
		  char *result, unsigned int maxlen)
{
	struct ploop *ploop = ti->private;
	bool read, forward = true;
	int ival, ret = -EPERM;
	u64 val, val2;

	if (!capable(CAP_SYS_ADMIN))
		goto out;

	ret = -EINVAL;
	if (argc < 1)
		goto out;

	read = msg_wants_down_read(argv[0]);
	if (read)
		down_read(&ploop->ctl_rwsem);
	else
		down_write(&ploop->ctl_rwsem);

	if (!strcmp(argv[0], "resize")) {
		if (argc != 2 || kstrtou64(argv[1], 10, &val) < 0)
			goto unlock;
		ret = ploop_resize(ploop, val);
	} else if (!strcmp(argv[0], "add_delta")) {
		if (argc != 2)
			goto unlock;
		ret = ploop_add_delta(ploop, argv[1]);
	} else if (!strcmp(argv[0], "merge")) {
		if (argc == 1)
			ret = ploop_merge_latest_snapshot(ploop);
	} else if (!strncmp(argv[0], "notify_merged_", 14)) {
		if (!strcmp(&argv[0][14], "backward"))
			forward = false;
		else if (strcmp(&argv[0][14], "forward"))
			goto unlock;
		if (argc != 2 || kstrtou64(argv[1], 10, &val) < 0)
			goto unlock;
		ret = ploop_notify_merged(ploop, val, forward);
	} else if (!strcmp(argv[0], "get_delta_name")) {
		if (argc != 2 || kstrtou64(argv[1], 10, &val) < 0)
			goto unlock;
		ret = ploop_get_delta_name_cmd(ploop, (u8)val, result, maxlen);
	} else if (!strcmp(argv[0], "update_delta_index")) {
		if (argc != 3 || kstrtou64(argv[1], 10, &val) < 0)
			goto unlock;
		ret = ploop_update_delta_index(ploop, val, argv[2]);
	} else if (!strcmp(argv[0], "snapshot")) {
		if (argc != 3 || kstrtou64(argv[1], 10, &val) < 0)
			goto unlock;
		ret = ploop_switch_top_delta(ploop, val, argv[2]);
	} else if (!strncmp(argv[0], "tracking_", 9)) {
		if (argc != 1)
			goto unlock;
		ret = ploop_tracking_cmd(ploop, argv[0] + 9, result, maxlen);
	} else if (!strcmp(argv[0], "set_noresume")) {
		if (argc != 2)
			goto unlock;
		ret = ploop_set_noresume(ploop, argv[1]);
	} else if (!strcmp(argv[0], "flip_upper_deltas")) {
		if (argc != 3)
			goto unlock;
		ret = ploop_flip_upper_deltas(ploop, argv[1], argv[2]);
	} else if (!strcmp(argv[0], "push_backup_start")) {
		if (argc != 3 || kstrtou64(argv[2], 10, &val) < 0)
			goto unlock;
		ret = ploop_push_backup_start(ploop, argv[1], (void *)val);
	} else if (!strcmp(argv[0], "push_backup_stop")) {
		if (argc != 3 || kstrtos32(argv[2], 10, &ival) < 0)
			goto unlock;
		ret = ploop_push_backup_stop(ploop, argv[1], ival,
					     result, maxlen);
	} else if (!strcmp(argv[0], "push_backup_get_uuid")) {
		if (argc != 1)
			goto unlock;
		ret = ploop_push_backup_get_uuid(ploop, result, maxlen);
	} else if (!strcmp(argv[0], "push_backup_read")) {
		if (argc != 2)
			goto unlock;
		ret = ploop_push_backup_read(ploop, argv[1], result, maxlen);
	} else if (!strcmp(argv[0], "push_backup_write")) {
		if (argc != 3 || sscanf(argv[2], "%llu:%llu", &val, &val2) != 2)
			goto unlock;
		ret = ploop_push_backup_write(ploop, argv[1], val, val2);
	} else {
		ret = -ENOTSUPP;
	}

unlock:
	if (read)
		up_read(&ploop->ctl_rwsem);
	else
		up_write(&ploop->ctl_rwsem);
out:
	return ret;
}
