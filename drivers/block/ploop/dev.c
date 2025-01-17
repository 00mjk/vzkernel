#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/bio.h>
#include <linux/interrupt.h>
#include <linux/buffer_head.h>
#include <linux/kthread.h>
#include <linux/statfs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/random.h>
#include <linux/ve.h>
#include <asm/uaccess.h>

#include <trace/events/block.h>

#include <linux/ploop/ploop.h>
#include "ploop_events.h"
#include "freeblks.h"
#include "discard.h"

/* Structures and terms:
 *
 * ploop_device is root of everything.
 *	Normally we use local variable "plo" to refer to it.
 *
 * ploop_device -> list of ploop_delta's.
 *	Head of list is "top delta", tail of list is "root delta".
 *	"top delta" is delta, where all the modifications are written,
 *	"root delta" is base image. "Level" is distance from root.
 *
 * ploop_delta  -> { ops, priv } refers to particulat format of delta.
 *		-> ploop_io refers to image on disk.
 *
 * ploop_io	-> list of ploop_file, each file maps an area in image.
 *	*** Further is "ideal", right now we support only one ploop_file
 *	*** and we do not support creation of new ploop_file's.
 *		-> { ops , priv } generic image ops, mostly creation
 *		   of new chunks.
 *
 * ploop_file	-> { file, ops, priv } how we do real IO on this file.
 */

static int ploop_max __read_mostly = PLOOP_DEVICE_RANGE;
static int ploop_major __read_mostly = PLOOP_DEVICE_MAJOR;
int max_map_pages __read_mostly;

static long root_threshold __read_mostly = 2L * 1024 * 1024; /* 2GB in KB */
static long user_threshold __read_mostly = 4L * 1024 * 1024; /* 4GB in KB */

static int large_disk_support __read_mostly = 1; /* true */

static struct rb_root ploop_devices_tree = RB_ROOT;
static DEFINE_MUTEX(ploop_devices_mutex);

static LIST_HEAD(ploop_formats);
static DEFINE_MUTEX(ploop_formats_mutex);

int ploop_register_format(struct ploop_delta_ops * ops)
{
	mutex_lock(&ploop_formats_mutex);
	list_add(&ops->list, &ploop_formats);
	mutex_unlock(&ploop_formats_mutex);
	return 0;
}
EXPORT_SYMBOL(ploop_register_format);

void ploop_unregister_format(struct ploop_delta_ops * ops)
{
	mutex_lock(&ploop_formats_mutex);
	list_del(&ops->list);
	mutex_unlock(&ploop_formats_mutex);
}
EXPORT_SYMBOL(ploop_unregister_format);

struct ploop_delta_ops * ploop_format_get(unsigned int id)
{
	struct ploop_delta_ops * ops;

	mutex_lock(&ploop_formats_mutex);
	list_for_each_entry(ops, &ploop_formats, list) {
		if (ops->id == id && try_module_get(ops->owner)) {
			mutex_unlock(&ploop_formats_mutex);
			return ops;
		}
	}
	mutex_unlock(&ploop_formats_mutex);
	return NULL;
}

void ploop_format_put(struct ploop_delta_ops * ops)
{
	module_put(ops->owner);
}

void ploop_msg_once(struct ploop_device *plo, const char *fmt, ...)
{
	va_list args;

	if (test_and_set_bit(PLOOP_S_ONCE, &plo->state))
		return;

	va_start(args, fmt);
	printk("ploop(%d): ", plo->index);
	vprintk(fmt, args);
	printk("\n");
	va_end(args);
}
EXPORT_SYMBOL(ploop_msg_once);

static void mitigation_timeout(unsigned long data)
{
	struct ploop_device * plo = (void*)data;

	if (!test_bit(PLOOP_S_RUNNING, &plo->state))
		return;

	spin_lock_irq(&plo->lock);
	if (test_bit(PLOOP_S_WAIT_PROCESS, &plo->state) &&
	    (!list_empty(&plo->entry_queue) ||
	     ((plo->bio_head || !bio_list_empty(&plo->bio_discard_list)) &&
	      !list_empty(&plo->free_list))) &&
	      waitqueue_active(&plo->waitq))
		wake_up_interruptible(&plo->waitq);
	spin_unlock_irq(&plo->lock);
}

static void freeze_timeout(unsigned long data)
{
	struct ploop_device * plo = (void*)data;

	spin_lock_irq(&plo->lock);
	if (waitqueue_active(&plo->freeze_waitq))
		wake_up_interruptible(&plo->freeze_waitq);
	spin_unlock_irq(&plo->lock);
}

static void ploop_congest(struct ploop_device *plo)
{
	if (!test_bit(PLOOP_S_CONGESTED, &plo->state) &&
	    PLOOP_CONGESTED(plo) > plo->tune.congestion_high_watermark)
		set_bit(PLOOP_S_CONGESTED, &plo->state);
}

static void ploop_uncongest(struct ploop_device *plo)
{
	if (PLOOP_CONGESTED(plo) <= plo->tune.congestion_low_watermark &&
	    test_and_clear_bit(PLOOP_S_CONGESTED, &plo->state)) {
		struct backing_dev_info *bdi = &plo->queue->backing_dev_info;

		if (waitqueue_active(&bdi->cong_waitq))
			wake_up_all(&bdi->cong_waitq);
	}
}

static struct ploop_request *
ploop_alloc_request(struct ploop_device * plo)
{
	struct ploop_request * preq;

	/* We allow only finite amount of request in process.
	 * If caller does not stop to congest us, we force him to wait.
	 *
	 * _XXX_ I am afraid this logic is flawed. The justification is
	 * that conventional devices, using request queues, do similar thing
	 * blocking in add_request(), but I am still not sure that logic
	 * applies here.
	 */
	if (list_empty(&plo->free_list)) {
		DEFINE_WAIT(_wait);
		for (;;) {
			prepare_to_wait(&plo->req_waitq, &_wait, TASK_UNINTERRUPTIBLE);
			if (!list_empty(&plo->free_list))
				break;
			plo->st.bio_full++;
			spin_unlock_irq(&plo->lock);
			io_schedule();
			spin_lock_irq(&plo->lock);
		}
		finish_wait(&plo->req_waitq, &_wait);
	}

	preq = list_entry(plo->free_list.next, struct ploop_request, list);
	list_del_init(&preq->list);
	ploop_congest(plo);
	return preq;
}

static void ploop_grab_iocontext(struct bio *bio)
{
	struct io_context **ioc_pp = (struct io_context **)(&bio->bi_bdev);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
	/* This is WRONG. Seems, in 2.6.18 we can do nothing.
	 * But leave this piece of art for now.
	 */
	if (current->io_context) {
		*ioc_pp = current->io_context;
		if (!atomic_inc_not_zero((*ioc_pp)->refcount))
			*ioc_pp = NULL;
		set_bit(BIO_BDEV_REUSED, &bio->bi_flags);
	}
#else
	if (current->io_context) {
		ioc_task_link(current->io_context);
		*ioc_pp = current->io_context;
		set_bit(BIO_BDEV_REUSED, &bio->bi_flags);
	}
#endif
}

/* always called with plo->lock held */
static inline void preq_unlink(struct ploop_request * preq,
			       struct list_head *drop_list)
{
	list_del(&preq->list);
	ploop_entry_qlen_dec(preq);
	list_add(&preq->list, drop_list);
}

/* always called with plo->lock released */
void ploop_preq_drop(struct ploop_device * plo, struct list_head *drop_list,
		      int keep_locked)
{
	struct ploop_request * preq;

	list_for_each_entry(preq, drop_list, list) {
		if (preq->ioc) {
			atomic_dec(&preq->ioc->nr_tasks);
			put_io_context_active(preq->ioc);
			preq->ioc = NULL;
		}

		BUG_ON (test_bit(PLOOP_REQ_ZERO, &preq->state));
	}

	spin_lock_irq(&plo->lock);

	list_splice_init(drop_list, plo->free_list.prev);
	if (waitqueue_active(&plo->req_waitq))
		wake_up(&plo->req_waitq);
	else if (test_bit(PLOOP_S_WAIT_PROCESS, &plo->state) &&
		waitqueue_active(&plo->waitq) &&
		(plo->bio_head || !bio_list_empty(&plo->bio_discard_list)))
		wake_up_interruptible(&plo->waitq);

	ploop_uncongest(plo);

	if (!keep_locked)
		spin_unlock_irq(&plo->lock);
}

static void merge_rw_flags_to_req(unsigned long rw,
				  struct ploop_request * preq)
{
		if (rw & REQ_FLUSH)
			preq->req_rw |= REQ_FLUSH;
		if (rw & REQ_FUA)
			preq->req_rw |= REQ_FUA;
}

static void preq_set_sync_bit(struct ploop_request * preq)
{
	if (!test_bit(PLOOP_REQ_SYNC, &preq->state)) {
		if (!(preq->req_rw & WRITE) || (preq->req_rw & (REQ_FLUSH|REQ_FUA))) {
			preq->plo->read_sync_reqs++;
			__set_bit(PLOOP_REQ_RSYNC, &preq->state);
		}
		__set_bit(PLOOP_REQ_SYNC, &preq->state);
	}
}

static void overlap_forward(struct ploop_device * plo,
			    struct ploop_request * preq,
			    struct ploop_request * preq1,
			    struct list_head *drop_list)
{
	struct rb_node * n;

	if (preq->req_sector + preq->req_size == preq1->req_sector) {
		preq->bl.tail->bi_next = preq1->bl.head;
		preq->bl.tail = preq1->bl.tail;
		preq1->bl.head = preq1->bl.tail = NULL;
		preq->req_size += preq1->req_size;
		if (test_bit(PLOOP_REQ_SYNC, &preq1->state))
			preq_set_sync_bit(preq);
		merge_rw_flags_to_req(preq1->req_rw, preq);
		rb_erase(&preq1->lockout_link, &plo->entry_tree[preq1->req_rw & WRITE]);
		preq_unlink(preq1, drop_list);
		plo->st.coal_mforw++;
	}

	while ((n = rb_next(&preq->lockout_link)) != NULL) {
		preq1 = rb_entry(n, struct ploop_request, lockout_link);
		if (preq->req_sector + preq->req_size <= preq1->req_sector)
			break;
		rb_erase(n, &plo->entry_tree[preq->req_rw & WRITE]);
		__clear_bit(PLOOP_REQ_SORTED, &preq1->state);
		plo->st.coal_oforw++;
	}
}

static void overlap_backward(struct ploop_device * plo,
			     struct ploop_request * preq,
			     struct ploop_request * preq1,
			     struct list_head *drop_list)
{
	struct rb_node * n;

	if (preq1->req_sector + preq1->req_size == preq->req_sector) {
		preq1->bl.tail->bi_next = preq->bl.head;
		preq->bl.head = preq1->bl.head;
		preq1->bl.head = preq1->bl.tail = NULL;
		preq->req_size += preq1->req_size;
		preq->req_sector = preq1->req_sector;
		if (test_bit(PLOOP_REQ_SYNC, &preq1->state))
			preq_set_sync_bit(preq);
		merge_rw_flags_to_req(preq1->req_rw, preq);
		rb_erase(&preq1->lockout_link, &plo->entry_tree[preq->req_rw & WRITE]);
		preq_unlink(preq1, drop_list);
		plo->st.coal_mback++;
	}

	while ((n = rb_prev(&preq->lockout_link)) != NULL) {
		preq1 = rb_entry(n, struct ploop_request, lockout_link);
		if (preq1->req_sector + preq1->req_size <= preq->req_sector)
			break;
		rb_erase(n, &plo->entry_tree[preq->req_rw & WRITE]);
		__clear_bit(PLOOP_REQ_SORTED, &preq1->state);
		plo->st.coal_oback++;
	}
}

static int try_merge(struct ploop_device *plo, struct ploop_request * preq,
		     struct bio * bio, struct list_head *drop_list)
{
	struct rb_node * n;

	/* Merge to tail */
	if (bio->bi_sector == preq->req_sector + preq->req_size) {
		preq->bl.tail->bi_next = bio;
		preq->bl.tail = bio;
		preq->req_size += (bio->bi_size >> 9);
		preq->tstamp = jiffies;
		if (bio->bi_rw & REQ_SYNC)
			preq_set_sync_bit(preq);
		merge_rw_flags_to_req(bio->bi_rw, preq);
		plo->st.coal_forw++;
		n = rb_next(&preq->lockout_link);
		if (n) {
			struct ploop_request * preq1;

			preq1 = rb_entry(n, struct ploop_request, lockout_link);
			if (preq1->req_cluster == preq->req_cluster &&
			    preq->req_sector + preq->req_size >= preq1->req_sector)
				overlap_forward(plo, preq, preq1, drop_list);
		}
		return 1;
	}

	if (bio->bi_sector + (bio->bi_size >> 9) == preq->req_sector) {
		bio->bi_next = preq->bl.head;
		preq->bl.head = bio;
		preq->req_size += (bio->bi_size >> 9);
		preq->req_sector = bio->bi_sector;
		preq->tstamp = jiffies;
		plo->st.coal_back++;
		if (bio->bi_rw & REQ_SYNC)
			preq_set_sync_bit(preq);
		merge_rw_flags_to_req(bio->bi_rw, preq);
		n = rb_prev(&preq->lockout_link);
		if (n) {
			struct ploop_request * preq1;

			preq1 = rb_entry(n, struct ploop_request, lockout_link);
			if (preq1->req_cluster == preq->req_cluster &&
			    preq->req_sector <= preq1->req_sector + preq1->req_size)
				overlap_backward(plo, preq, preq1, drop_list);
		}
		return 1;
	}

	return 0;
}

static struct ploop_request *
tree_insert(struct rb_root *root, struct ploop_request * preq0)
{
	struct rb_node ** p = &root->rb_node;
	struct rb_node * parent = NULL;
	struct ploop_request * preq;

	while (*p) {
		parent = *p;
		preq = rb_entry(parent, struct ploop_request, lockout_link);

		if (preq0->req_cluster < preq->req_cluster)
			p = &(*p)->rb_left;
		else if (preq0->req_cluster > preq->req_cluster)
			p = &(*p)->rb_right;
		else if (preq0->req_sector + preq0->req_size < preq->req_sector)
			p = &(*p)->rb_left;
		else if (preq0->req_sector > preq->req_sector + preq->req_size)
			p = &(*p)->rb_right;
		else
			return preq;
	}

	rb_link_node(&preq0->lockout_link, parent, p);
	rb_insert_color(&preq0->lockout_link, root);
	__set_bit(PLOOP_REQ_SORTED, &preq0->state);
	return NULL;
}

static int
insert_entry_tree(struct ploop_device * plo, struct ploop_request * preq0,
		  struct list_head *drop_list)
{
	struct ploop_request * clash;
	struct rb_node * n;

	clash = tree_insert(&plo->entry_tree[preq0->req_rw & WRITE], preq0);
	if (!clash)
		return 0;

	if (preq0->req_sector == clash->req_sector + clash->req_size) {
		clash->bl.tail->bi_next = preq0->bl.head;
		clash->bl.tail = preq0->bl.tail;
		clash->req_size += preq0->req_size;
		clash->tstamp = jiffies;
		if (test_bit(PLOOP_REQ_SYNC, &preq0->state))
			preq_set_sync_bit(clash);
		merge_rw_flags_to_req(preq0->req_rw, clash);
		preq_unlink(preq0, drop_list);
		plo->st.coal_forw2++;

		n = rb_next(&clash->lockout_link);
		if (n) {
			struct ploop_request * preq1;

			preq1 = rb_entry(n, struct ploop_request, lockout_link);
			if (preq1->req_cluster == clash->req_cluster &&
			    clash->req_sector + clash->req_size >= preq1->req_sector)
				overlap_forward(plo, clash, preq1, drop_list);
		}
		return 1;
	}

	if (clash->req_sector == preq0->req_sector + preq0->req_size) {
		preq0->bl.tail->bi_next = clash->bl.head;
		clash->bl.head = preq0->bl.head;
		clash->req_size += preq0->req_size;
		clash->req_sector = preq0->req_sector;
		clash->tstamp = jiffies;
		plo->st.coal_back2++;
		if (test_bit(PLOOP_REQ_SYNC, &preq0->state))
			preq_set_sync_bit(clash);
		merge_rw_flags_to_req(preq0->req_rw, clash);
		preq_unlink(preq0, drop_list);

		n = rb_prev(&clash->lockout_link);
		if (n) {
			struct ploop_request * preq1;

			preq1 = rb_entry(n, struct ploop_request, lockout_link);
			if (preq1->req_cluster == clash->req_cluster &&
			    clash->req_sector <= preq1->req_sector + preq1->req_size)
				overlap_backward(plo, clash, preq1, drop_list);
		}
		return 1;
	}

	plo->st.coal_overlap++;

	return 0;
}

static void
ploop_bio_queue(struct ploop_device * plo, struct bio * bio,
		struct list_head *drop_list)
{
	struct ploop_request * preq;

	BUG_ON (list_empty(&plo->free_list));
	preq = list_entry(plo->free_list.next, struct ploop_request, list);
	list_del_init(&preq->list);

	preq->req_cluster = bio->bi_sector >> plo->cluster_log;
	bio->bi_next = NULL;
	preq->req_sector = bio->bi_sector;
	preq->req_size = bio->bi_size >> 9;
	preq->req_rw = bio->bi_rw;
	bio->bi_rw &= ~(REQ_FLUSH | REQ_FUA);
	preq->eng_state = PLOOP_E_ENTRY;
	preq->state = 0;
	preq->error = 0;
	preq->tstamp = jiffies;
	preq->iblock = 0;
	preq->prealloc_size = 0;

	if (unlikely(bio->bi_rw & REQ_DISCARD)) {
		int clu_size = 1 << plo->cluster_log;
		int i = (clu_size - 1) & bio->bi_sector;
		int err = 0;

		if (i) {
			preq->req_cluster++;
			if (preq->req_size >= clu_size)
				preq->req_size -= clu_size - i;
		}

		if (preq->req_size < clu_size ||
		    (err = ploop_discard_add_bio(plo->fbd, bio))) {
			if (test_bit(BIO_BDEV_REUSED, &bio->bi_flags)) {
				struct io_context *ioc;
				ioc = (struct io_context *)(bio->bi_bdev);
				atomic_dec(&ioc->nr_tasks);
				put_io_context_active(ioc);

				bio->bi_bdev = plo->bdev;
				clear_bit(BIO_BDEV_REUSED, &bio->bi_flags);
			}
			BIO_ENDIO(plo->queue, bio, err);
			list_add(&preq->list, &plo->free_list);
			plo->bio_qlen--;
			plo->bio_discard_qlen--;
			plo->bio_total--;
			return;
		}

		preq->state = (1 << PLOOP_REQ_SYNC) | (1 << PLOOP_REQ_DISCARD);
		preq->dst_iblock = 0;
		preq->bl.head = preq->bl.tail = NULL;
	} else
		preq->bl.head = preq->bl.tail = bio;

	if (test_bit(BIO_BDEV_REUSED, &bio->bi_flags)) {
		    preq->ioc = (struct io_context *)(bio->bi_bdev);
		    bio->bi_bdev = plo->bdev;
		    clear_bit(BIO_BDEV_REUSED, &bio->bi_flags);
	} else {
		preq->ioc = NULL;
	}

	if (unlikely(bio->bi_rw & REQ_SYNC))
		__set_bit(PLOOP_REQ_SYNC, &preq->state);
	if (unlikely(bio == plo->bio_sync)) {
		__set_bit(PLOOP_REQ_SYNC, &preq->state);
		plo->bio_sync = NULL;
	}

	__TRACE("A %p %u\n", preq, preq->req_cluster);

	if (unlikely(bio->bi_rw & REQ_DISCARD))
		plo->bio_discard_qlen--;
	else
		plo->bio_qlen--;
	ploop_entry_add(plo, preq);

	if (bio->bi_size && !(bio->bi_rw & REQ_DISCARD))
		insert_entry_tree(plo, preq, drop_list);

	trace_bio_queue(preq);
}

static inline struct ploop_request *
ploop_get_request(struct ploop_device * plo, struct list_head * list)
{
	struct ploop_request * preq;

	if (unlikely(list_empty(list)))
		return NULL;

	preq = list_first_entry(list, struct ploop_request, list);
	list_del_init(&preq->list);
	return preq;
}

static struct ploop_delta * find_delta(struct ploop_device * plo, int level)
{
	struct ploop_delta * delta;

	list_for_each_entry(delta, &plo->map.delta_list, list) {
		if (delta->level == level)
			return delta;
	}

	return NULL;
}

DEFINE_BIO_CB(ploop_fast_end_io)
{
	unsigned long flags;
	struct ploop_device * plo;
	struct bio * orig = bio->bi_private;

	plo = orig->bi_bdev->bd_disk->private_data;

	BIO_ENDIO(plo->queue, orig, err);

	/* End of fast bio wakes up main process only when this could
	 * mean exit from ATTENTION state.
	 */
	spin_lock_irqsave(&plo->lock, flags);
	plo->active_reqs--;
	plo->fastpath_reqs--;
	plo->bio_total--;

	if (plo->active_reqs == 0 &&
	    test_bit(PLOOP_S_WAIT_PROCESS, &plo->state) &&
	    waitqueue_active(&plo->waitq) &&
	    (test_bit(PLOOP_S_EXITING, &plo->state) ||
	     !list_empty(&plo->entry_queue)))
		wake_up_interruptible(&plo->waitq);
	spin_unlock_irqrestore(&plo->lock, flags);

	bio_put(bio);
}
END_BIO_CB(ploop_fast_end_io)

static struct ploop_delta *
ploop_fast_lookup(struct ploop_device * plo, sector_t sec,
		  unsigned long rw, sector_t * isec)
{
	struct ploop_delta * top_delta, * delta;
	int level;
	cluster_t bio_cluster = sec >> plo->cluster_log;
	iblock_t iblk;

	level = ploop_fastmap(&plo->map, bio_cluster, &iblk);
	if (level < 0)
		return NULL;

	top_delta = ploop_top_delta(plo);
	delta = top_delta;

	if (level != top_delta->level) {
		/* _XXX_ here is a problem. While merge_bvec() we do
		 * not know whether this bio is read or write. If it is read
		 * we should check backing map. This is tradeoff:
		 * either we will direct reads to slow path, or we
		 * do not aggregate writes, which makes COW much
		 * slower. For now we select optimization of COW.
		 */
		if (rw & REQ_WRITE)
			return NULL;

		delta = find_delta(plo, level);
	}
	if (delta) {
		*isec = ((sector_t)iblk << plo->cluster_log) +
			(sec & ((1 << plo->cluster_log) - 1));
	}
	return delta;
}


/* Got a bio, which is mapped 1-1 to block device.
 * But there is a problem, this bio could bypass device merge functions,
 * because we skipped it while our own merge_fn.
 *
 * We cannot split bio in fast path, but we can revalidate it.
 *
 * q->max_phys_segments and q->max_hw_segments must be set to minimal
 * of all participating backing devices.
 */

static int
bio_fast_map(struct ploop_device * plo, struct bio * orig_bio, struct bio * bio)
{
	struct ploop_delta * delta;
	sector_t isector;

	if (orig_bio->bi_size == 0)
		delta = ploop_top_delta(plo);
	else
		delta = ploop_fast_lookup(plo, orig_bio->bi_sector,
					  orig_bio->bi_rw, &isector);
	if (delta == NULL) {
		plo->st.fast_neg_nomap++;
		return 1;
	}

	if (delta->io.ops->fastmap == NULL)
		return 1;

	return delta->io.ops->fastmap(&delta->io, orig_bio, bio, isector);
}

static inline unsigned int block_vecs(struct ploop_device * plo)
{
	return 1 << (plo->cluster_log + 9 - PAGE_SHIFT);
}

static int whole_block(struct ploop_device * plo, struct ploop_request *preq)
{
	if (preq->req_size != (1<<plo->cluster_log))
		return 0;
	return !(preq->req_sector & ((1<<plo->cluster_log) - 1));
}

static struct bio *
preallocate_bio(struct bio * orig_bio, struct ploop_device * plo)
{
	struct bio * nbio = NULL;

	if (plo->cached_bio) {
		spin_lock_irq(&plo->lock);
		nbio = plo->cached_bio;
		if (nbio) {
			if (orig_bio->bi_vcnt <= nbio->bi_max_vecs)
				plo->cached_bio = NULL;
			else
				nbio = NULL;
		}
		spin_unlock_irq(&plo->lock);
	}

	if (nbio == NULL)
		nbio = bio_alloc(GFP_NOFS, max(orig_bio->bi_max_vecs, block_vecs(plo)));
	return nbio;
}

static void process_bio_queue(struct ploop_device * plo, struct list_head *drop_list)
{
	while (plo->bio_head && !list_empty(&plo->free_list)) {
		struct bio *tmp = plo->bio_head;

		BUG_ON (!plo->bio_tail);
		plo->bio_head = plo->bio_head->bi_next;
		if (!plo->bio_head)
			plo->bio_tail = NULL;

		ploop_bio_queue(plo, tmp, drop_list);
	}
}

static void ploop_unplug(struct blk_plug_cb *cb, bool from_schedule)
{
	struct ploop_device *plo = cb->data;

	clear_bit(PLOOP_S_SYNC, &plo->state);

	/* And kick our "soft" queue too in case mitigation timer is in effect */
	spin_lock_irq(&plo->lock);
	if (plo->bio_head) {
		BUG_ON (!plo->bio_tail);
		/* another way would be: bio_tail->bi_rw |= BIO_RW_SYNCIO; */
		plo->bio_sync = plo->bio_tail;
	} else if (!list_empty(&plo->entry_queue)) {
		struct ploop_request * preq = list_entry(plo->entry_queue.prev,
							 struct ploop_request,
							 list);
		preq_set_sync_bit(preq);
	}

	if ((!list_empty(&plo->entry_queue) ||
	     (plo->bio_head && !list_empty(&plo->free_list))) &&
	    test_bit(PLOOP_S_WAIT_PROCESS, &plo->state) &&
	    waitqueue_active(&plo->waitq))
		wake_up_interruptible(&plo->waitq);
	spin_unlock_irq(&plo->lock);

	kfree(cb);
}

static void
process_discard_bio_queue(struct ploop_device * plo, struct list_head *drop_list)
{
	bool discard = test_bit(PLOOP_S_DISCARD, &plo->state);

	while (!list_empty(&plo->free_list)) {
		struct bio *tmp;

		/* Only one discard bio can be handled concurrently */
		if (discard && ploop_discard_is_inprogress(plo->fbd))
			return;

		tmp = bio_list_pop(&plo->bio_discard_list);
		if (tmp == NULL)
			break;

		/* If PLOOP_S_DISCARD isn't set, ploop_bio_queue
		 * will complete it with a proper error.
		 */
		ploop_bio_queue(plo, tmp, drop_list);
	}
}

static void ploop_make_request(struct request_queue *q, struct bio *bio)
{
	struct bio * nbio;
	struct ploop_device * plo = q->queuedata;
	unsigned long rw;
	LIST_HEAD(drop_list);

	trace_make_request(bio);

	plo->st.bio_in++;

	BUG_ON(bio->bi_idx);
	BUG_ON(bio->bi_size & 511);

	if (unlikely(bio->bi_size == 0)) {
		/* Is it possible? This makes sense if the request is
		 * marked as FLUSH, otherwise just warn and complete. */
		if (!(bio->bi_rw & REQ_FLUSH)) {
			WARN_ON(1);
			BIO_ENDIO(q, bio, 0);
			return;
		}
		/* useless to pass this bio further */
		if (!plo->tune.pass_flushes) {
			ploop_acc_ff_in(plo, bio->bi_rw);
			BIO_ENDIO(q, bio, 0);
			return;
		}
	}

	/* This is crazy. Pattern is borrowed from raid0.c
	 * bio layer assumes that it can prepare single-page bio
	 * not depending on any alignment constraints. So be it.
	 */
	if (!(bio->bi_rw & REQ_DISCARD) && bio->bi_size &&
	    (bio->bi_sector >> plo->cluster_log) !=
	    ((bio->bi_sector + (bio->bi_size >> 9) - 1) >> plo->cluster_log)) {
		struct bio_pair *bp;
		unsigned int first_sectors = (1<<plo->cluster_log)
			- (bio->bi_sector & ((1<<plo->cluster_log) - 1));

		plo->st.bio_splits++;

		BUG_ON(bio->bi_vcnt != 1 || bio->bi_idx != 0);

		bp = bio_split(bio, first_sectors);
		ploop_make_request(q, &bp->bio1);
		ploop_make_request(q, &bp->bio2);
		bio_pair_release(bp);
		return;
	}

	rw = bio->bi_rw;
	if (unlikely((bio->bi_rw & REQ_FLUSH) &&
		     !plo->tune.pass_flushes))
		bio->bi_rw &= ~REQ_FLUSH;
	if (unlikely((bio->bi_rw & REQ_FUA) &&
		     !plo->tune.pass_fuas))
		bio->bi_rw &= ~REQ_FUA;

	/* Allocate new bio now. */
	nbio = preallocate_bio(bio, plo);

	if (!current->io_context) {
		struct io_context *ioc;
		ioc = get_task_io_context(current, GFP_NOFS, NUMA_NO_NODE);
		if (ioc)
			put_io_context(ioc);
	}

	spin_lock_irq(&plo->lock);
	ploop_acc_ff_in_locked(plo, rw);
	plo->bio_total++;

	/* Device is aborted, everything is in error. This should not happen. */
	if (unlikely(!test_bit(PLOOP_S_RUNNING, &plo->state) ||
		     ((bio->bi_rw & REQ_WRITE) &&
		      test_bit(PLOOP_S_ABORT, &plo->state)))) {
		plo->bio_total--;
		spin_unlock_irq(&plo->lock);

		BIO_ENDIO(q, bio, -EIO);
		if (nbio)
			bio_put(nbio);
		return;
	}

	if (bio->bi_rw & REQ_DISCARD) {
		bio_list_add(&plo->bio_discard_list, bio);
		plo->bio_discard_qlen++;
		goto queued;
	}

	/* Write tracking in fast path does not work at the moment. */
	if (unlikely(test_bit(PLOOP_S_TRACK, &plo->state) &&
		     (bio->bi_rw & WRITE)))
		goto queue;

	/* No fast path, when maintenance is in progress.
	 * (PLOOP_S_TRACK was checked immediately above) */
	if (FAST_PATH_DISABLED(plo->maintenance_type))
		goto queue;

	/* Attention state, always queue */
	if (unlikely(test_bit(PLOOP_S_ATTENTION, &plo->state)))
		goto queue;

	/* Some barriers have been already enqueued, always queue */
	if (unlikely(plo->barrier_reqs))
		goto queue;

	if (unlikely(nbio == NULL))
		goto queue;

	/* Try to merge before checking for fastpath. Maybe, this
	 * is not wise.
	 */
	if (!RB_EMPTY_ROOT(&plo->entry_tree[bio->bi_rw & WRITE]) &&
	    bio->bi_size) {
		struct ploop_request * preq;
		struct rb_node * n = plo->entry_tree[bio->bi_rw & WRITE].rb_node;
		u32 bio_cluster = bio->bi_sector >> plo->cluster_log;

		while (n) {
			preq = rb_entry(n, struct ploop_request, lockout_link);

			if (bio_cluster < preq->req_cluster)
				n = n->rb_left;
			else if (bio_cluster > preq->req_cluster)
				n = n->rb_right;
			else if (bio->bi_sector + (bio->bi_size >> 9) < preq->req_sector)
				n = n->rb_left;
			else if (bio->bi_sector > preq->req_sector + preq->req_size)
				n = n->rb_right;
			else
				break;
		}

		if (n && try_merge(plo, preq, bio, &drop_list))
			goto out;
	}


	/* Try fast path. If all the mappings are available
	 * and bio can be remapped without split, just do it.
	 */
	if (!bio_fast_map(plo, bio, nbio)) {
		/* Here is a little problem. It would be really good
		 * to remap original bio and to return 1. It is how
		 * make_request() engine is supposed to work.
		 * Nevertheless, this logic is flawed.
		 *
		 * We cannot return remapped bio, because we lose track of it
		 * and have no way to wait for end of IO f.e. to start
		 * snapshot or to replace image file.
		 */
		trace_bio_fast_map(bio);
		nbio->bi_private = bio;
		nbio->bi_end_io = ploop_fast_end_io;
		plo->active_reqs++;
		plo->fastpath_reqs++;
		plo->st.bio_fast++;
		ploop_acc_ff_out_locked(plo, nbio->bi_rw);

		spin_unlock_irq(&plo->lock);

		generic_make_request(nbio);
		return;
	}

	/* Otherwise: queue */

queue:
	BUG_ON (bio->bi_bdev != plo->bdev && bio_sectors(bio));
	if (bio->bi_bdev == plo->bdev) {
		BUG_ON (test_bit(BIO_BDEV_REUSED, &bio->bi_flags));
		ploop_grab_iocontext(bio);
	}

	BUG_ON (bio->bi_next);
	if (plo->bio_tail) {
		BUG_ON (!plo->bio_head);
		BUG_ON (plo->bio_tail->bi_next);
		plo->bio_tail->bi_next = bio;
		plo->bio_tail = bio;
	} else {
		BUG_ON (plo->bio_head);
		plo->bio_head = plo->bio_tail = bio;
	}
	plo->bio_qlen++;
	ploop_congest(plo);

	/* second chance to merge requests */
	process_bio_queue(plo, &drop_list);

queued:
	/* If main thread is waiting for requests, wake it up.
	 * But try to mitigate wakeups, delaying wakeup for some short
	 * time.
	 */
	if (test_bit(PLOOP_S_WAIT_PROCESS, &plo->state)) {
		/* Synchronous requests are not batched. */
		if (plo->entry_qlen > plo->tune.batch_entry_qlen ||
			(bio->bi_rw & (REQ_FLUSH|REQ_FUA)) ||
			(!bio_list_empty(&plo->bio_discard_list) &&
			 !list_empty(&plo->free_list)) ||
			!current->plug) {
			wake_up_interruptible(&plo->waitq);
		} else if (!timer_pending(&plo->mitigation_timer)) {
			mod_timer(&plo->mitigation_timer,
				  jiffies + plo->tune.batch_entry_delay);
		}
	}
out:
	if (nbio) {
		if (!plo->cached_bio)
			plo->cached_bio = nbio;
		else
			bio_put(nbio);
	}
	spin_unlock_irq(&plo->lock);

	blk_check_plugged(ploop_unplug, plo, sizeof(struct blk_plug_cb));

	if (!list_empty(&drop_list))
		ploop_preq_drop(plo, &drop_list, 0);

	return;
}


/* q->merge_bvec_fn
 *
 * According to API, this function returns length which we are able
 * to merge, but nobody uses it actually, so that we return either 0
 * or bvec->bv_len.
 */

static int
ploop_merge_bvec(struct request_queue *q, struct bvec_merge_data *bm_data,
		 struct bio_vec *bvec)
{
	struct ploop_device *plo = q->queuedata;
	struct ploop_delta * delta;
	sector_t sec;
	sector_t isector;
	unsigned int len, ret;
	unsigned long flags;

	sec = bm_data->bi_sector + get_start_sect(bm_data->bi_bdev);
	len = bm_data->bi_size + bvec->bv_len;
	ret = bvec->bv_len;

	/* Always allow to add the first bvec. */
	if (!bm_data->bi_size)
		return ret;

	/* Is this possible? This would not contradict to anything. */
	BUG_ON(len & 511);

	len >>= 9;

	if ((sec >> plo->cluster_log) != 
	    ((sec + len - 1) >> plo->cluster_log)) {
		plo->st.merge_neg_cluster++;
		return 0;
	}

	/* We can return ret right now, the further action is an optimization
	 * to prevent splitting overhead and to enable fast path.
	 */
	spin_lock_irqsave(&plo->lock, flags);
	delta = ploop_fast_lookup(plo, sec, 0, &isector);
	if (delta &&
	    delta->io.ops->disable_merge &&
	    delta->io.ops->disable_merge(&delta->io, isector, len)) {
		plo->st.merge_neg_disable++;
		ret = 0;
	}
	spin_unlock_irqrestore(&plo->lock, flags);

	/* If no mapping is available, merge up to cluster boundary */
	return ret;
}

static int ploop_congested2(void *data, int bits)
{
	struct ploop_device * plo = data;

	if (test_bit(PLOOP_S_CONGESTED, &plo->state))
		return bits;

	return 0;
}

static int ploop_congested(void *data, int bits)
{
	struct ploop_device * plo = data;
	struct ploop_delta * top_delta;
	int ret = 0;

	top_delta = ploop_top_delta(plo);
	if (top_delta->io.ops->congested)
		ret |= top_delta->io.ops->congested(&top_delta->io, bits);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24) && LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)
	if (plo->tune.congestion_detection &&
	    plo->entry_qlen + plo->active_reqs - plo->fastpath_reqs
	    > (3*plo->tune.max_requests)/4) {
		if ((bits & (1<<BDI_write_congested)) && !(ret & (1<<BDI_write_congested))) {
			ret |= (1<<BDI_write_congested);
			set_bit(PLOOP_S_WRITE_CONG, &plo->state);
		}
		if ((bits & (1<<BDI_read_congested)) && !(ret & (1<<BDI_read_congested))) {
			ret |= (1<<BDI_read_congested);
			set_bit(PLOOP_S_READ_CONG, &plo->state);
		}
	}
#endif

	return ret;
}

static int check_lockout(struct ploop_request *preq)
{
	struct ploop_device * plo = preq->plo;
	struct rb_node * n = plo->lockout_tree.rb_node;
	struct ploop_request * p;

	if (n == NULL)
		return 0;

	if (test_bit(PLOOP_REQ_LOCKOUT, &preq->state))
		return 0;

	while (n) {
		p = rb_entry(n, struct ploop_request, lockout_link);

		if (preq->req_cluster < p->req_cluster)
			n = n->rb_left;
		else if (preq->req_cluster > p->req_cluster)
			n = n->rb_right;
		else {
			list_add_tail(&preq->list, &p->delay_list);
			plo->st.bio_lockouts++;
			trace_preq_lockout(preq, p);
			return 1;
		}
	}
	return 0;
}

int ploop_add_lockout(struct ploop_request *preq, int try)
{
	struct ploop_device * plo = preq->plo;
	struct rb_node ** p = &plo->lockout_tree.rb_node;
	struct rb_node *parent = NULL;
	struct ploop_request * pr;

	if (test_bit(PLOOP_REQ_LOCKOUT, &preq->state))
		return 0;

	while (*p) {
		parent = *p;
		pr = rb_entry(parent, struct ploop_request, lockout_link);

		if (preq->req_cluster == pr->req_cluster) {
			if (try)
				return 1;
			BUG();
		}

		if (preq->req_cluster < pr->req_cluster)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	trace_add_lockout(preq);

	rb_link_node(&preq->lockout_link, parent, p);
	rb_insert_color(&preq->lockout_link, &plo->lockout_tree);
	__set_bit(PLOOP_REQ_LOCKOUT, &preq->state);
	return 0;
}
EXPORT_SYMBOL(ploop_add_lockout);

void del_lockout(struct ploop_request *preq)
{
	struct ploop_device * plo = preq->plo;

	if (!test_and_clear_bit(PLOOP_REQ_LOCKOUT, &preq->state))
		return;

	trace_del_lockout(preq);

	rb_erase(&preq->lockout_link, &plo->lockout_tree);
}

static void ploop_discard_wakeup(struct ploop_request *preq, int err)
{
	struct ploop_device *plo = preq->plo;

	if (err || !ploop_fb_get_n_free(plo->fbd)) {
		/* Only one discard request is processed */
		ploop_fb_reinit(plo->fbd, err);
	} else
		set_bit(PLOOP_S_DISCARD_LOADED, &plo->state);

	if (atomic_dec_and_test(&plo->maintenance_cnt))
		if (test_bit(PLOOP_S_DISCARD_LOADED, &plo->state) ||
		    !test_bit(PLOOP_S_DISCARD, &plo->state))
			complete(&plo->maintenance_comp);
}

static void ploop_complete_request(struct ploop_request * preq)
{
	struct ploop_device * plo = preq->plo;
	int nr_completed = 0;
	struct io_context *ioc;

	trace_complete_request(preq);

	__TRACE("Z %p %u\n", preq, preq->req_cluster);

	while (preq->bl.head) {
		struct bio * bio = preq->bl.head;
		preq->bl.head = bio->bi_next;
		bio->bi_next = NULL;
		BIO_ENDIO(plo->queue, bio, preq->error);
		nr_completed++;
	}
	preq->bl.tail = NULL;

	if (test_bit(PLOOP_REQ_RELOC_A, &preq->state) ||
	    test_bit(PLOOP_REQ_RELOC_S, &preq->state)) {
		if (preq->error)
			set_bit(PLOOP_S_ABORT, &plo->state);

		if (atomic_dec_and_test(&plo->maintenance_cnt))
			complete(&plo->maintenance_comp);
	} else if (test_bit(PLOOP_REQ_MERGE, &preq->state)) {
		if (!preq->error) {
			if (plo->merge_ptr < plo->trans_map->max_index) {
				spin_lock_irq(&plo->lock);
				if (preq->map) {
					map_release(preq->map);
					preq->map = NULL;
				}
				if (preq->trans_map) {
					map_release(preq->trans_map);
					preq->trans_map = NULL;
				}

				del_lockout(preq);

				preq->req_cluster = ~0U;

				if (!list_empty(&preq->delay_list))
					list_splice_init(&preq->delay_list, plo->ready_queue.prev);
				plo->active_reqs--;

				preq->eng_state = PLOOP_E_ENTRY;
				ploop_entry_add(plo, preq);
				spin_unlock_irq(&plo->lock);
				return;
			}
		} else
			set_bit(PLOOP_S_ABORT, &plo->state);

		if (atomic_dec_and_test(&plo->maintenance_cnt))
			complete(&plo->maintenance_comp);
	} else if (test_bit(PLOOP_REQ_DISCARD, &preq->state))
		ploop_discard_wakeup(preq, preq->error);

	if (preq->aux_bio) {
		int i;
		struct bio * bio = preq->aux_bio;

		for (i = 0; i < bio->bi_vcnt; i++)
			put_page(bio->bi_io_vec[i].bv_page);

		bio_put(bio);

		preq->aux_bio = NULL;
	}

	spin_lock_irq(&plo->lock);

	del_lockout(preq);

	if (!list_empty(&preq->delay_list))
		list_splice_init(&preq->delay_list, plo->ready_queue.prev);

	if (preq->map) {
		map_release(preq->map);
		preq->map = NULL;
	}
	if (preq->trans_map) {
		map_release(preq->trans_map);
		preq->trans_map = NULL;
	}

	ioc = preq->ioc;
	preq->ioc = NULL;

	plo->active_reqs--;

	if (unlikely(test_bit(PLOOP_REQ_ZERO, &preq->state))) {
		ploop_fb_put_zero_request(plo->fbd, preq);
	} else {
		ploop_uncongest(plo);
		list_add(&preq->list, &plo->free_list);
		if (waitqueue_active(&plo->req_waitq))
			wake_up(&plo->req_waitq);
		else if (test_bit(PLOOP_S_WAIT_PROCESS, &plo->state) &&
			 waitqueue_active(&plo->waitq) &&
			 (plo->bio_head ||
			  !bio_list_empty(&plo->bio_discard_list)))
			wake_up_interruptible(&plo->waitq);
	}
	plo->bio_total -= nr_completed;

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
	if (plo->tune.congestion_detection &&
	    plo->entry_qlen + plo->active_reqs - plo->fastpath_reqs
	    <= plo->tune.max_requests/2) {
		if (test_and_clear_bit(PLOOP_S_WRITE_CONG, &plo->state))
			clear_bdi_congested(&plo->queue->backing_dev_info, WRITE);
		if (test_and_clear_bit(PLOOP_S_READ_CONG, &plo->state))
			clear_bdi_congested(&plo->queue->backing_dev_info, READ);
	}
#endif

	spin_unlock_irq(&plo->lock);

	if (ioc) {
		atomic_dec(&ioc->nr_tasks);
		put_io_context_active(ioc);
	}
}

void ploop_fail_request(struct ploop_request * preq, int err)
{
	struct ploop_device * plo = preq->plo;

	ploop_req_set_error(preq, err);

	spin_lock_irq(&plo->lock);
	if (err == -ENOSPC) {
		set_bit(PLOOP_S_ENOSPC_EVENT, &plo->state);
		list_add(&preq->list, &plo->ready_queue);
		if (waitqueue_active(&plo->event_waitq))
			wake_up_interruptible(&plo->event_waitq);
	} else {
		set_bit(PLOOP_S_ABORT, &plo->state);
		list_add_tail(&preq->list, &plo->ready_queue);
	}
	spin_unlock_irq(&plo->lock);
}
EXPORT_SYMBOL(ploop_fail_request);

void ploop_fail_immediate(struct ploop_request * preq, int err)
{
	struct ploop_device * plo = preq->plo;

	ploop_req_set_error(preq, err);

	set_bit(PLOOP_S_ABORT, &plo->state);
	preq->eng_state = PLOOP_E_COMPLETE;
	ploop_complete_request(preq);
}

#define PLOOP_REQ_FAIL_IMMEDIATE(preq, err)		\
	do {						\
		PLOOP_REQ_TRACE_ERROR(preq, err);	\
		ploop_fail_immediate(preq, err);	\
	} while (0);

void ploop_complete_io_state(struct ploop_request * preq)
{
	struct ploop_device * plo = preq->plo;
	unsigned long flags;

	spin_lock_irqsave(&plo->lock, flags);
	__TRACE("C %p %u\n", preq, preq->req_cluster);
	if (preq->error)
		set_bit(PLOOP_S_ABORT, &plo->state);

	list_add_tail(&preq->list, &plo->ready_queue);
	if (test_bit(PLOOP_S_WAIT_PROCESS, &plo->state) &&
	    waitqueue_active(&plo->waitq))
		wake_up_interruptible(&plo->waitq);
	spin_unlock_irqrestore(&plo->lock, flags);
}
EXPORT_SYMBOL(ploop_complete_io_state);


static int fill_bio(struct ploop_device *plo, struct bio * bio, cluster_t blk)
{
	int pages = block_vecs(plo);

	for (; bio->bi_vcnt < pages; bio->bi_vcnt++) {
		bio->bi_io_vec[bio->bi_vcnt].bv_page = alloc_page(GFP_NOFS);
		if (bio->bi_io_vec[bio->bi_vcnt].bv_page == NULL)
			return -ENOMEM;
		bio->bi_io_vec[bio->bi_vcnt].bv_offset = 0;
		bio->bi_io_vec[bio->bi_vcnt].bv_len = PAGE_SIZE;
	}
	bio->bi_sector = blk << plo->cluster_log;
	bio->bi_size = (1 << (plo->cluster_log + 9));
	return 0;
}

/* Not generic. We assume that dst is aligned properly, i.e. it is
 * array of the whole pages starting at cluster boundary.
 */
static void bio_bcopy(struct bio *dst, struct bio *src, struct ploop_device *plo)
{
	int i;
	unsigned int doff, soff, bv_off;

	doff = (src->bi_sector & ((1<<plo->cluster_log) - 1)) << 9;
	soff = 0;
	bv_off = 0;
	i = 0;

	while (soff < src->bi_size) {
		struct bio_vec * bv = src->bi_io_vec + i;
		unsigned int copy;
		int didx;
		int poff;
		void * ksrc;

		if (bv_off >= bv->bv_len) {
			i++;
			bv++;
			bv_off = 0;
		}

		didx = doff / PAGE_SIZE;
		poff = doff & (PAGE_SIZE-1);
		copy = bv->bv_len - bv_off;
		if (copy > PAGE_SIZE - poff)
			copy = PAGE_SIZE - poff;

		ksrc = kmap_atomic(bv->bv_page);
		memcpy(page_address(dst->bi_io_vec[didx].bv_page) + poff,
		       ksrc + bv->bv_offset + bv_off,
		       copy);
		kunmap_atomic(ksrc);

		bv_off += copy;
		doff += copy;
		soff += copy;
	}
}

int check_zeros(struct bio_list * bl)
{
	struct bio * bio;

	bio_list_for_each(bio, bl) {
		int i;

		for (i = 0; i < bio->bi_vcnt; i++) {
			struct bio_vec * bv = bio->bi_io_vec + i;
			unsigned long * ptr;
			void * kaddr;
			int k;

			if (bv->bv_page == ZERO_PAGE(0))
				continue;

			kaddr = kmap_atomic(bv->bv_page);
			ptr = kaddr + bv->bv_offset;
			k = bv->bv_len/sizeof(unsigned long);
			while (k) {
				if (*ptr)
					break;
				ptr++;
				k--;
			}
			kunmap_atomic(kaddr);
			if (k)
				return 0;
		}
	}
	return 1;
}

static int prepare_merge_req(struct ploop_request * preq)
{
	struct ploop_device * plo = preq->plo;
	u32 iblk;
	int res;

	BUG_ON (preq->trans_map == NULL);

	if (trans_map_get_index(preq, preq->req_cluster, &iblk)) {
		u32 cluster = preq->req_cluster;

		preq->req_cluster = ~0U;

		if (cluster + 1 != plo->merge_ptr)
			goto drop_map;

		do {
			cluster++;

			if (cluster >= plo->trans_map->max_index)
				goto drop_map;

			if (cluster > map_get_mn_end(preq->trans_map)) {
				plo->merge_ptr = cluster;
				goto drop_map;
			}
		} while (trans_map_get_index(preq, cluster, &iblk));

		preq->req_cluster = cluster;
		plo->merge_ptr = cluster + 1;
	}

	spin_lock_irq(&plo->lock);
	res = ploop_add_lockout(preq, 1);
	spin_unlock_irq(&plo->lock);
	return res;

drop_map:
	spin_lock_irq(&plo->lock);
	map_release(preq->trans_map);
	preq->trans_map = NULL;
	if (preq->map) {
		map_release(preq->map);
		preq->map = NULL;
	}
	spin_unlock_irq(&plo->lock);
	return 1;
}

void ploop_queue_zero_request(struct ploop_device *plo,
			      struct ploop_request *orig_preq, cluster_t clu)
{
	struct ploop_request * preq;

	spin_lock_irq(&plo->lock);

	preq = ploop_fb_get_zero_request(plo->fbd);
	preq->bl.tail = preq->bl.head = NULL;
	preq->req_cluster = clu;
	preq->req_size = 0;
	preq->req_rw = WRITE_SYNC;
	preq->eng_state = PLOOP_E_ENTRY;
	preq->state = (1 << PLOOP_REQ_ZERO);
	if (test_bit(PLOOP_REQ_SYNC, &orig_preq->state))
		preq->state |= (1 << PLOOP_REQ_SYNC);
	preq->error = 0;
	preq->tstamp = jiffies;
	preq->iblock = 0;

	if (test_bit(PLOOP_REQ_RELOC_S, &orig_preq->state)) {
		if (orig_preq->dst_iblock == ~0U)
			orig_preq->eng_state = PLOOP_E_RELOC_COMPLETE;
	} else {
		orig_preq->eng_state = orig_preq->iblock ?
			PLOOP_E_DELTA_ZERO_INDEX : PLOOP_E_ZERO_INDEX;
	}
	orig_preq->iblock = 0;
	INIT_LIST_HEAD(&preq->delay_list);
	list_add_tail(&orig_preq->list, &preq->delay_list);

	list_add(&preq->list, &plo->ready_queue);
	plo->active_reqs++;

	spin_unlock_irq(&plo->lock);
}

static void
ploop_reloc_sched_read(struct ploop_request *preq, iblock_t iblk)
{
	struct ploop_device *plo   = preq->plo;
	struct ploop_delta  *delta = ploop_top_delta(plo);
	struct bio_list sbl;

	spin_lock_irq(&plo->lock);
	if (check_lockout(preq)) {
		__TRACE("l2 %p %u\n", preq, preq->req_cluster);
		spin_unlock_irq(&plo->lock);
		return;
	}
	ploop_add_lockout(preq, 0);
	spin_unlock_irq(&plo->lock);

	if (!preq->aux_bio) {
		preq->aux_bio = bio_alloc(GFP_NOFS, block_vecs(plo));

		if (!preq->aux_bio ||
		    fill_bio(plo, preq->aux_bio, preq->req_cluster)) {
			PLOOP_REQ_FAIL_IMMEDIATE(preq, -ENOMEM);
			return;
		}
	}

	preq->iblock = iblk;
	preq->eng_state = PLOOP_E_RELOC_DATA_READ;
	sbl.head = sbl.tail = preq->aux_bio;
	delta->io.ops->submit(&delta->io, preq, READ_SYNC,
			      &sbl, iblk, 1<<plo->cluster_log);
}

/*
 * Returns 0 if and only if a free block was successfully reused
 */
static int
ploop_reuse_free_block(struct ploop_request *preq)
{
	struct ploop_device *plo       = preq->plo;
	struct ploop_delta  *top_delta = ploop_top_delta(plo);
	iblock_t  iblk;
	cluster_t clu;
	int	  rc;
	unsigned long pin_state;

	if (plo->maintenance_type != PLOOP_MNTN_FBLOADED &&
	    plo->maintenance_type != PLOOP_MNTN_RELOC)
		return -1;

	rc = ploop_fb_get_free_block(plo->fbd, &clu, &iblk);

	/* simple case - no free blocks left */
	if (rc < 0)
		return rc;

	/* a free block to reuse requires zeroing index */
	if (rc > 0) {
		ploop_queue_zero_request(plo, preq, clu);
		return 0;
	}

	/* 'rc == 0' - use iblk as a lost block */
	pin_state = preq->iblock ? PLOOP_E_DELTA_ZERO_INDEX :
				   PLOOP_E_ZERO_INDEX;
	preq->iblock = iblk;

	/* pin preq to some reloc request processing iblk ? */
	if (ploop_fb_check_reloc_req(plo->fbd, preq, pin_state))
		return 0;

	/* iblk is a lost block and nobody is relocating it now */
	preq->eng_state = PLOOP_E_DATA_WBI;
	__TRACE("T2 %p %u\n", preq, preq->req_cluster);
	plo->st.bio_out++;

	if (pin_state == PLOOP_E_ZERO_INDEX) {
		top_delta->io.ops->submit(&top_delta->io, preq, preq->req_rw,
					  &preq->bl, preq->iblock,
					  preq->req_size);
	} else { /* PLOOP_E_DELTA_READ */
		struct bio_list sbl;

		BUG_ON (preq->aux_bio == NULL);
		sbl.head = sbl.tail = preq->aux_bio;

		top_delta->io.ops->submit(&top_delta->io, preq, preq->req_rw,
				      &sbl, preq->iblock, 1<<plo->cluster_log);
	}

	return 0;
}

/*
 * Returns 0 if and only if zero preq was successfully processed
 */
static int
ploop_entry_zero_req(struct ploop_request *preq)
{
	struct ploop_device *plo       = preq->plo;
	struct ploop_delta  *top_delta = ploop_top_delta(plo);
	int	 level;
	iblock_t iblk = 0;
	int	 err;

	err = ploop_find_map(&plo->map, preq);
	if (err) {
		if (err == 1) {
			__TRACE("m %p %u\n", preq, *clu);
			return 0;
		}
		return err;
	}

	level = map_get_index(preq, preq->req_cluster, &iblk);
	if (level != top_delta->level) {
		printk("Can't zero index on wrong level=%d "
		       "(top_level=%d req_cluster=%u iblk=%u/%u)\n",
		       level, top_delta->level, preq->req_cluster,
		       iblk, preq->iblock);
		return -EIO;
	}

	ploop_index_update(preq);
	return 0;
}

#define MAP_MAX_IND(preq) min(map_get_mn_end(preq->map),	\
			      preq->plo->map.max_index - 1)

/*
 * Returns 0 if and only if RELOC_A preq was successfully processed.
 *
 * Advance preq->req_cluster till it points to *iblk in grow range.
 * Returning 0, always set *iblk to a meaningful value: either zero
 * (if preq->req_cluster went out of allowed range or map is being read)
 * or iblock in grow range that preq->req_cluster points to.
 */
static int
ploop_entry_reloc_a_req(struct ploop_request *preq, iblock_t *iblk)
{
	struct ploop_device *plo       = preq->plo;
	struct ploop_delta  *top_delta = ploop_top_delta(plo);
	cluster_t           *clu       = &preq->req_cluster;
	int level;
	int err;
	BUG_ON (*clu == ~0U);

	while(*clu < plo->map.max_index) {
		err = ploop_find_map(&plo->map, preq);
		if (err) {
			if (err == 1) {
				__TRACE("m %p %u\n", preq, *clu);
				*iblk = 0;
				return 0;
			}
			return err;
		}
		BUG_ON (preq->map == NULL);

		for (; *clu <= MAP_MAX_IND(preq); (*clu)++) {
			level = map_get_index(preq, *clu, iblk);
			if (level == top_delta->level &&
			    *iblk >= plo->grow_start &&
			    *iblk <= plo->grow_end)
				break;
		}

		if (*clu <= MAP_MAX_IND(preq))
			break;

		spin_lock_irq(&plo->lock);
		map_release(preq->map);
		preq->map = NULL;
		spin_unlock_irq(&plo->lock);
	}

	if (*clu >= plo->map.max_index) {
		preq->eng_state = PLOOP_E_COMPLETE;
		ploop_complete_request(preq);
		*iblk = 0;
		return 0;
	}

	return 0;
}

/*
 * Returns 0 if and only if RELOC_S preq was successfully processed.
 *
 * Sets preq->req_cluster to the block we're going to relocate.
 * Returning 0, always set *iblk to a meaningful value: either
 * zero (if no more blocks to relocate or block to relocate is free
 *	 (and zero-index op is scheduled) or map is being read)
 * or iblock that preq->req_cluster points to.
 */
static int
ploop_entry_reloc_s_req(struct ploop_request *preq, iblock_t *iblk)
{
	struct ploop_device *plo       = preq->plo;
	struct ploop_delta  *top_delta = ploop_top_delta(plo);

	cluster_t from_clu, to_clu;
	iblock_t from_iblk, to_iblk;
	u32 free;
	int level;
	int err;

	*iblk = 0;

	if (preq->req_cluster == ~0U) {
		cluster_t zero_cluster;

		BUG_ON (preq->error);
		err = ploop_fb_get_reloc_block(plo->fbd, &from_clu, &from_iblk,
					       &to_clu, &to_iblk, &free);
		if (err < 0) {
			preq->eng_state = PLOOP_E_COMPLETE;
			ploop_complete_request(preq);
			return 0;
		}

		preq->req_cluster = from_clu;
		preq->src_iblock  = from_iblk;
		ploop_fb_add_reloc_req(plo->fbd, preq);

		if (free) {
			preq->dst_iblock  = ~0U;
			preq->dst_cluster = ~0U;
			zero_cluster = preq->req_cluster;
		} else {
			preq->dst_iblock  = to_iblk;
			preq->dst_cluster = to_clu;
			zero_cluster = preq->dst_cluster;
		}

		ploop_queue_zero_request(plo, preq, zero_cluster);
		return 0;
	}

	err = ploop_find_map(&plo->map, preq);
	if (err) {
		if (err == 1) {
			__TRACE("m %p %u\n", preq, *clu);
			return 0;
		}
		return err;
	}
	BUG_ON (preq->map == NULL);

	level = map_get_index(preq, preq->req_cluster, iblk);
	if (level != top_delta->level) {
		printk("Can't relocate block on wrong level=%d "
		       "(top_level=%d req_cluster=%u iblk=%u/%u)\n",
		       level, top_delta->level, preq->req_cluster,
		       *iblk, preq->iblock);
		return -EIO;
	}
	if (preq->src_iblock != *iblk) {
		printk("Can't relocate block due to wrong mapping: "
		       "req_cluster=%u should point to iblk=%u while "
		       "map_get_index() calculated iblk=%u\n",
		       preq->req_cluster, preq->src_iblock, *iblk);
		return -EIO;
	}

	return 0;
}

/* dummy wrapper around ploop_entry_reloc_[a|s]_req() */
static int
ploop_entry_reloc_req(struct ploop_request *preq, iblock_t *iblk)
{
	if (test_bit(PLOOP_REQ_RELOC_A, &preq->state))
		return ploop_entry_reloc_a_req(preq, iblk);
	else if (test_bit(PLOOP_REQ_RELOC_S, &preq->state))
		return ploop_entry_reloc_s_req(preq, iblk);
	else
		BUG();
}

static int discard_get_index(struct ploop_request *preq)
{
	struct ploop_device *plo       = preq->plo;
	struct ploop_delta  *top_delta = ploop_top_delta(plo);
	int	 level;
	int	 err;

	preq->iblock = 0;

	err = ploop_find_map(&plo->map, preq);
	if (err)
		return err;

	level = map_get_index(preq, preq->req_cluster, &preq->iblock);
	if (level != top_delta->level)
		preq->iblock = 0;

	if (preq->map) {
		spin_lock_irq(&plo->lock);
		map_release(preq->map);
		preq->map = NULL;
		spin_unlock_irq(&plo->lock);
	}

	return 0;
}

static int ploop_entry_discard_req(struct ploop_request *preq)
{
	int err = 0;
	struct ploop_device * plo = preq->plo;
	unsigned int len = 0;
	cluster_t last_clu;

	if (!test_bit(PLOOP_S_DISCARD, &plo->state)) {
		err = -EOPNOTSUPP;
		goto err;
	}

	BUG_ON(plo->maintenance_type != PLOOP_MNTN_DISCARD);

	last_clu = (preq->req_sector + preq->req_size) >> plo->cluster_log;

	for (; preq->req_cluster < last_clu; preq->req_cluster++) {
		len = preq->req_cluster - preq->dst_cluster;

		err = discard_get_index(preq);
		if (err) {
			if (err == 1)
				return 0;
			goto err;
		}

		if (preq->dst_iblock &&
		    (!preq->iblock || preq->dst_iblock + len != preq->iblock)) {
			err = ploop_fb_add_free_extent(plo->fbd,
							preq->dst_cluster,
							preq->dst_iblock, len);
			preq->dst_iblock = 0;
			if (err)
				goto err;
		}

		if (!preq->dst_iblock && preq->iblock) {
			preq->dst_cluster = preq->req_cluster;
			preq->dst_iblock = preq->iblock;
		}
	}

	if (preq->dst_iblock) {
		len = preq->req_cluster - preq->dst_cluster;
		err = ploop_fb_add_free_extent(plo->fbd, preq->dst_cluster,
						preq->dst_iblock, len);
	}

err:
	preq->error = err;
	preq->eng_state = PLOOP_E_COMPLETE;
	ploop_complete_request(preq);

	return 0;
}

/* Main preq state machine */

static void
ploop_entry_request(struct ploop_request * preq)
{
	struct ploop_device * plo       = preq->plo;
	struct ploop_delta  * top_delta = ploop_top_delta(plo);
	struct ploop_delta  * delta;
	int level;
	int err;
	iblock_t iblk;

	/* Control request. */
	if (unlikely(preq->bl.head == NULL &&
		     !test_bit(PLOOP_REQ_MERGE, &preq->state) &&
		     !test_bit(PLOOP_REQ_RELOC_A, &preq->state) &&
		     !test_bit(PLOOP_REQ_RELOC_S, &preq->state) &&
		     !test_bit(PLOOP_REQ_DISCARD, &preq->state) &&
		     !test_bit(PLOOP_REQ_ZERO, &preq->state))) {
		complete(plo->quiesce_comp);
		wait_for_completion(&plo->relax_comp);
		ploop_complete_request(preq);
		complete(&plo->relaxed_comp);
		return;
	}

	/* Empty flush. */
	if (unlikely(preq->req_size == 0 &&
		     !test_bit(PLOOP_REQ_MERGE, &preq->state) &&
		     !test_bit(PLOOP_REQ_RELOC_A, &preq->state) &&
		     !test_bit(PLOOP_REQ_RELOC_S, &preq->state) &&
		     !test_bit(PLOOP_REQ_ZERO, &preq->state))) {
		if (preq->req_rw & REQ_FLUSH) {
			if (top_delta->io.ops->issue_flush) {
				top_delta->io.ops->issue_flush(&top_delta->io, preq);
				return;
			}
		}

		preq->eng_state = PLOOP_E_COMPLETE;
		ploop_complete_request(preq);
		return;
	}

	if (unlikely(test_bit(PLOOP_REQ_SYNC, &preq->state) &&
		     !(preq->req_rw & REQ_SYNC)))
		preq->req_rw |= REQ_SYNC;

restart:
	if (test_bit(PLOOP_REQ_DISCARD, &preq->state)) {
		err = ploop_entry_discard_req(preq);
		if (err)
			goto error;
		return;
	} else if (test_bit(PLOOP_REQ_ZERO, &preq->state)) {
		err = ploop_entry_zero_req(preq);
		if (err)
			goto error;
		return;
	} else if (test_bit(PLOOP_REQ_RELOC_A, &preq->state) ||
		   test_bit(PLOOP_REQ_RELOC_S, &preq->state)) {
		err = ploop_entry_reloc_req(preq, &iblk);
		if (err)
			goto error;
		if (iblk)
			ploop_reloc_sched_read(preq, iblk);
		return;
	} else if (preq->req_cluster == ~0U) {
		BUG_ON(!test_bit(PLOOP_REQ_MERGE, &preq->state));
		BUG_ON(preq->trans_map);
		BUG_ON(preq->map);

		preq->req_cluster = plo->merge_ptr;
		plo->merge_ptr++;
		if (preq->req_cluster >= plo->trans_map->max_index) {
			preq->eng_state = PLOOP_E_COMPLETE;
			ploop_complete_request(preq);
			return;
		}
	}

	if (check_lockout(preq)) {
		__TRACE("l %p %u\n", preq, preq->req_cluster);
		return;
	}

	if (plo->trans_map) {
		err = ploop_find_trans_map(plo->trans_map, preq);
		if (err) {
			if (err == 1) {
				__TRACE("tm %p %u\n", preq, preq->req_cluster);
				return;
			}
			goto error;
		}

		if (preq->trans_map &&
		    !(preq->req_rw & REQ_WRITE) &&
		    trans_map_get_index(preq, preq->req_cluster, &iblk) == 0) {
			delta = map_top_delta(plo->trans_map);
			preq->iblock = iblk;
			preq->eng_state = PLOOP_E_COMPLETE;
			plo->st.bio_out++;
			__TRACE("tS %p %u\n", preq, preq->req_cluster);
			delta->io.ops->submit(&delta->io, preq, preq->req_rw, &preq->bl,
					      iblk, preq->req_size);
			return;
		}

		if (test_bit(PLOOP_REQ_MERGE, &preq->state)) {
			if (prepare_merge_req(preq))
				goto restart;
		}
	}

	err = ploop_find_map(&plo->map, preq);
	if (err) {
		if (err == 1) {
			__TRACE("m %p %u\n", preq, preq->req_cluster);
			return;
		}
		goto error;
	}

	if (preq->trans_map &&
	    trans_map_get_index(preq, preq->req_cluster, &iblk) == 0) {
		struct bio_list sbl;

		/* Read requests were served earlier. */
		BUG_ON(!(preq->req_rw & REQ_WRITE));

		spin_lock_irq(&plo->lock);
		ploop_add_lockout(preq, 0);
		spin_unlock_irq(&plo->lock);

		if (whole_block(plo, preq)) {
			set_bit(PLOOP_REQ_TRANS, &preq->state);
			plo->st.bio_trans_whole++;
			goto delta_io;
		}

		plo->st.bio_cows++;

		if (!preq->aux_bio)
			preq->aux_bio = bio_alloc(GFP_NOFS, block_vecs(plo));

		if (!preq->aux_bio ||
		    fill_bio(plo, preq->aux_bio, preq->req_cluster)) {
			PLOOP_REQ_FAIL_IMMEDIATE(preq, -ENOMEM);
			return;
		}

		delta = map_top_delta(plo->trans_map);

		__TRACE("tDR %p %u\n", preq, preq->req_cluster);
		preq->iblock = iblk;
		preq->eng_state = PLOOP_E_TRANS_DELTA_READ;
		sbl.head = sbl.tail = preq->aux_bio;
		delta->io.ops->submit(&delta->io, preq, READ_SYNC,
				      &sbl, iblk, 1<<plo->cluster_log);
		plo->st.bio_trans_copy++;
		return;
	}

delta_io:
	BUG_ON(test_bit(PLOOP_REQ_MERGE, &preq->state));

	delta = top_delta;

	level = map_get_index(preq, preq->req_cluster, &iblk);
	if (level < 0) {
		delta = NULL;
	} else if (level != top_delta->level) {
		delta = find_delta(plo, level);
		if (!delta) {
			err = -EIO;
			goto error;
		}
	}

	if (!(preq->req_rw & REQ_WRITE)) {
		/* Read direction. If we found existing block in some
		 * delta, we direct bio there. If we did not, this location
		 * was never written before. We return zero fill and,
		 * probably, should log an alert.
		 */
		if (!delta) {
			struct bio * bio;

			if (map_index_fault(preq) == 0) {
				__TRACE("i %p %u\n", preq, preq->req_cluster);
				return;
			}

			__TRACE("X %p %u\n", preq, preq->req_cluster);
			bio_list_for_each(bio, &preq->bl) {
				zero_fill_bio(bio);
			}
			ploop_complete_request(preq);
			plo->st.bio_rzero++;
			return;
		}
		preq->iblock = iblk;
		preq->eng_state = PLOOP_E_COMPLETE;
		plo->st.bio_out++;
		__TRACE("S %p %u\n", preq, preq->req_cluster);
		delta->io.ops->submit(&delta->io, preq, preq->req_rw, &preq->bl,
				      iblk, preq->req_size);
	} else {
		if (delta) {
			if (delta == top_delta) {
				/* Block exists in top delta. Good. */
				if (plo->maintenance_type == PLOOP_MNTN_GROW ||
				    plo->maintenance_type == PLOOP_MNTN_RELOC) {
					spin_lock_irq(&plo->lock);
					ploop_add_lockout(preq, 0);
					spin_unlock_irq(&plo->lock);
				}
				preq->iblock = iblk;
				preq->eng_state = PLOOP_E_COMPLETE;
				__TRACE("T %p %u\n", preq, preq->req_cluster);
				plo->st.bio_out++;
				delta->io.ops->submit(&delta->io, preq, preq->req_rw,
						      &preq->bl, iblk, preq->req_size);
			} else if (whole_block(plo, preq)) {
				__TRACE("O1 %p %u\n", preq, preq->req_cluster);
				/* Block does not exist in top delta,
				 * but it exists in some delta.
				 * BUT! Plain luck, we have full block
				 * and can skip read stage.
				 */
				plo->st.bio_whole_cows++;

				/* About lockout. Reads could proceed
				 * without lockout.
				 */
				spin_lock_irq(&plo->lock);
				ploop_add_lockout(preq, 0);
				spin_unlock_irq(&plo->lock);

				if (likely(ploop_reuse_free_block(preq)))
					top_delta->ops->allocate(top_delta,
								 preq, &preq->bl,
								 preq->req_size);
			} else {
				struct bio_list sbl;

				plo->st.bio_cows++;

				if (!preq->aux_bio)
					preq->aux_bio = bio_alloc(GFP_NOFS, block_vecs(plo));

				if (!preq->aux_bio ||
				    fill_bio(plo, preq->aux_bio, preq->req_cluster)) {
					PLOOP_REQ_FAIL_IMMEDIATE(preq, -ENOMEM);
					return;
				}
				spin_lock_irq(&plo->lock);
				ploop_add_lockout(preq, 0);
				spin_unlock_irq(&plo->lock);

				__TRACE("DR %p %u\n", preq, preq->req_cluster);
				preq->iblock = iblk;
				preq->eng_state = PLOOP_E_DELTA_READ;
				sbl.head = sbl.tail = preq->aux_bio;
				delta->io.ops->submit(&delta->io, preq, READ_SYNC,
						      &sbl, iblk, 1<<plo->cluster_log);
			}
		} else {
			if (!whole_block(plo, preq) && map_index_fault(preq) == 0) {
					__TRACE("f %p %u\n", preq, preq->req_cluster);
					return;
			}

			if (plo->tune.check_zeros && check_zeros(&preq->bl)) {
				if (map_index_fault(preq) == 0) {
					__TRACE("f %p %u\n", preq, preq->req_cluster);
					return;
				}
				preq->eng_state = PLOOP_E_COMPLETE;
				/* Not ploop_complete_request().
				 * This can be TRANS request.
				 */
				ploop_complete_io_state(preq);
				if(whole_block(plo, preq))
					plo->st.bio_alloc_whole++;
				plo->st.bio_wzero++;
				return;
			}
			if(whole_block(plo, preq))
				plo->st.bio_alloc_whole++;

			spin_lock_irq(&plo->lock);
			ploop_add_lockout(preq, 0);
			spin_unlock_irq(&plo->lock);

			/* Block does not exist. */
			if (likely(ploop_reuse_free_block(preq))) {
				__TRACE("K %p %u\n", preq, preq->req_cluster);
				plo->st.bio_alloc++;
				top_delta->ops->allocate(top_delta, preq,
							 &preq->bl,
							 preq->req_size);
			}
		}
	}
	return;

error:
	PLOOP_REQ_FAIL_IMMEDIATE(preq, err);
}

static void ploop_req_state_process(struct ploop_request * preq)
{
	struct ploop_device * plo = preq->plo;
	struct ploop_delta * top_delta;
	struct io_context * saved_ioc = NULL;
	int release_ioc = 0;
#ifdef CONFIG_BEANCOUNTERS
	struct user_beancounter * uninitialized_var(saved_ub);
#endif

	trace_req_state_process(preq);

	if (preq->ioc) {
		saved_ioc = current->io_context;
		current->io_context = preq->ioc;
#ifdef CONFIG_BEANCOUNTERS
		saved_ub = set_exec_ub(preq->ioc->ioc_ub);
#endif
		atomic_long_inc(&preq->ioc->refcount);
		release_ioc = 1;
	}

	if (preq->eng_state != PLOOP_E_COMPLETE &&
	    test_bit(PLOOP_REQ_SYNC, &preq->state))
		set_bit(PLOOP_S_SYNC, &plo->state);

	if (test_bit(PLOOP_REQ_TRACK, &preq->state)) {
		sector_t sec;
		clear_bit(PLOOP_REQ_TRACK, &preq->state);

		sec = (sector_t)preq->track_cluster << plo->cluster_log;
		if (sec < plo->track_end)
			ploop_tracker_notify(plo, sec);
	}

restart:
	__TRACE("ST %p %u %lu\n", preq, preq->req_cluster, preq->eng_state);
	switch (preq->eng_state) {
	case PLOOP_E_ENTRY:
		/* First entry */
		if (preq->error ||
		    ((preq->req_rw & REQ_WRITE) &&
		     test_bit(PLOOP_S_ABORT, &plo->state))) {
			PLOOP_REQ_FAIL_IMMEDIATE(preq, preq->error ? : -EIO);
			break;
		}

		ploop_entry_request(preq);
		break;

	case PLOOP_E_RELOC_COMPLETE:
		BUG_ON (!test_bit(PLOOP_REQ_RELOC_S, &preq->state));
		if (!preq->error) {
			ploop_fb_relocate_req_completed(plo->fbd);
			ploop_fb_del_reloc_req(plo->fbd, preq);
			spin_lock_irq(&plo->lock);
			if (!list_empty(&preq->delay_list)) {
				struct ploop_request *pr;
				pr = list_entry(preq->delay_list.next,
						struct ploop_request, list);
				list_splice_init(&preq->delay_list,
						 plo->ready_queue.prev);
			}
			spin_unlock_irq(&plo->lock);
			preq->req_cluster = ~0U;
			preq->src_iblock  = ~0U; /* redundant */
			preq->dst_cluster = ~0U; /* redundant */
			preq->dst_iblock  = ~0U; /* redundant */
			preq->eng_state = PLOOP_E_ENTRY;
			goto restart;
		}
		/* drop down to PLOOP_E_COMPLETE case ... */
	case PLOOP_E_COMPLETE:
		if (unlikely(test_bit(PLOOP_REQ_RELOC_S, &preq->state) &&
			     preq->error)) {
			printk("RELOC_S completed with err %d"
			       " (%u %u %u %u %u)\n",
			       preq->error, preq->req_cluster, preq->iblock,
			       preq->src_iblock, preq->dst_cluster,
			       preq->dst_iblock);
			ploop_fb_del_reloc_req(plo->fbd, preq);
		}

		if (!preq->error &&
		    test_bit(PLOOP_REQ_TRANS, &preq->state)) {
			u32 iblk;

			__clear_bit(PLOOP_REQ_TRANS, &preq->state);
			BUG_ON(!preq->trans_map);
			if (!trans_map_get_index(preq, preq->req_cluster, &iblk)) {
				spin_lock_irq(&plo->lock);
				if (preq->map)
					map_release(preq->map);
				preq->map = preq->trans_map;
				preq->trans_map = NULL;
				spin_unlock_irq(&plo->lock);
				preq->iblock = 0;
				top_delta = map_top_delta(plo->trans_map);
				top_delta->ops->allocate_complete(top_delta, preq);
				plo->st.bio_trans_index++;
				break;
			}
		}

		ploop_complete_request(preq);
		/* All done. */
		break;

	case PLOOP_E_DELTA_READ:
	{
		struct bio * b;

		/* preq was scheduled for read from delta. bio is a bio
		 * covering full block of data. Now we should copy data
		 * and proceed with write.
		 */
		if (preq->error ||
		    test_bit(PLOOP_S_ABORT, &plo->state)) {
			PLOOP_REQ_FAIL_IMMEDIATE(preq, preq->error ? : -EIO);
			break;
		}

		bio_list_for_each(b, &preq->bl) {
			bio_bcopy(preq->aux_bio, b, plo);
		}

		/* Fall through ... */
	}
	case PLOOP_E_DELTA_COPIED:
	{
		if (likely(ploop_reuse_free_block(preq))) {
			struct bio_list sbl;
			sbl.head = sbl.tail = preq->aux_bio;
			top_delta = ploop_top_delta(plo);
			top_delta->ops->allocate(top_delta, preq,
						 &sbl, 1<<plo->cluster_log);
		}
		break;
	}
	case PLOOP_E_ZERO_INDEX:
	{
		preq->eng_state = PLOOP_E_DATA_WBI;
		top_delta = ploop_top_delta(plo);
		plo->st.bio_out++;
		if (whole_block(plo, preq)) {
			top_delta->io.ops->submit(&top_delta->io, preq, preq->req_rw,
						  &preq->bl, preq->iblock,
						  preq->req_size);
		} else {
			struct bio_list sbl;
			struct bio * b;
			int i;

			if (!preq->aux_bio)
				preq->aux_bio = bio_alloc(GFP_NOFS, block_vecs(plo));

			if (!preq->aux_bio ||
			    fill_bio(plo, preq->aux_bio, preq->req_cluster)) {
				PLOOP_REQ_FAIL_IMMEDIATE(preq, -ENOMEM);
				break;
			}

			for (i = 0; i < preq->aux_bio->bi_vcnt; i++)
				memset(page_address(preq->aux_bio->bi_io_vec[i].bv_page),
				       0, PAGE_SIZE);

			bio_list_for_each(b, &preq->bl) {
				bio_bcopy(preq->aux_bio, b, plo);
			}

			sbl.head = sbl.tail = preq->aux_bio;
			top_delta->io.ops->submit(&top_delta->io, preq, preq->req_rw,
						  &sbl, preq->iblock, 1<<plo->cluster_log);
		}
		break;
	}
	case PLOOP_E_DELTA_ZERO_INDEX:
	{
		struct bio_list sbl;

		BUG_ON (preq->aux_bio == NULL);

		preq->eng_state = PLOOP_E_DATA_WBI;
		sbl.head = sbl.tail = preq->aux_bio;
		top_delta = ploop_top_delta(plo);
		plo->st.bio_out++;
		top_delta->io.ops->submit(&top_delta->io, preq, preq->req_rw,
					  &sbl, preq->iblock,
					  1<<plo->cluster_log);
		break;
	}
	case PLOOP_E_RELOC_DATA_READ:
	{
		struct bio_list sbl;

		if (preq->error ||
		    test_bit(PLOOP_S_ABORT, &plo->state)) {
			PLOOP_REQ_FAIL_IMMEDIATE(preq, preq->error ? : -EIO);
			break;
		}

		BUG_ON (!preq->aux_bio);

		top_delta = ploop_top_delta(plo);
		sbl.head = sbl.tail = preq->aux_bio;

		/* Relocated data write required sync before BAT updatee */
		set_bit(PLOOP_REQ_FORCE_FUA, &preq->state);

		if (test_bit(PLOOP_REQ_RELOC_S, &preq->state)) {
			preq->eng_state = PLOOP_E_DATA_WBI;
			plo->st.bio_out++;
			preq->iblock = preq->dst_iblock;
			top_delta->io.ops->submit(&top_delta->io, preq,
						  preq->req_rw, &sbl,
						  preq->iblock,
						  1<<plo->cluster_log);
		} else {
			top_delta->ops->allocate(top_delta, preq, &sbl,
						 1<<plo->cluster_log);
		}
		break;
	}
	case PLOOP_E_RELOC_NULLIFY:
	{
		if (preq->error ||
		    test_bit(PLOOP_S_ABORT, &plo->state)) {
			PLOOP_REQ_FAIL_IMMEDIATE(preq, preq->error ? : -EIO);
			break;
		}

		BUG_ON (!preq->aux_bio);

		if (++plo->grow_relocated > plo->grow_end - plo->grow_start) {
			preq->eng_state = PLOOP_E_COMPLETE;
			ploop_complete_request(preq);
			break;
		}

		del_lockout(preq);
		preq->eng_state = PLOOP_E_ENTRY;
		preq->req_cluster++;
		goto restart;
	}
	case PLOOP_E_TRANS_DELTA_READ:
	{
		struct bio * b;
		struct bio_list sbl;
		u32 iblk;

		/* preq was scheduled for read from delta. bio is a bio
		 * covering full block of data. Now we should copy data
		 * and proceed with write.
		 */
		if (preq->error ||
		    test_bit(PLOOP_S_ABORT, &plo->state)) {
			PLOOP_REQ_FAIL_IMMEDIATE(preq, preq->error ? : -EIO);
			break;
		}

		bio_list_for_each(b, &preq->bl) {
			bio_bcopy(preq->aux_bio, b, plo);
		}

		top_delta = ploop_top_delta(plo);
		sbl.head = sbl.tail = preq->aux_bio;

		__set_bit(PLOOP_REQ_TRANS, &preq->state);
		if (map_get_index(preq, preq->req_cluster, &iblk) != top_delta->level) {
			/*
			 * we can be here only if merge is in progress and
			 * merge can't happen concurrently with ballooning
			 */
			top_delta->ops->allocate(top_delta, preq, &sbl, 1<<plo->cluster_log);
			plo->st.bio_trans_alloc++;
		} else {
			preq->eng_state = PLOOP_E_COMPLETE;
			preq->iblock = iblk;
			top_delta->io.ops->submit(&top_delta->io, preq, preq->req_rw,
						  &sbl, iblk, 1<<plo->cluster_log);
		}
		break;
	}
	case PLOOP_E_INDEX_READ:
	case PLOOP_E_TRANS_INDEX_READ:
		/* It was an index read. */
		map_read_complete(preq);
		preq->eng_state = PLOOP_E_ENTRY;
		goto restart;

	case PLOOP_E_DATA_WBI:
		/* Data written. Index must be updated. */
		if (preq->error ||
		    test_bit(PLOOP_S_ABORT, &plo->state)) {
			PLOOP_REQ_FAIL_IMMEDIATE(preq, preq->error ? : -EIO);
			break;
		}

		top_delta = ploop_top_delta(plo);
		top_delta->ops->allocate_complete(top_delta, preq);
		break;

	case PLOOP_E_INDEX_WB:
		/* Index write completed. */
		ploop_index_wb_complete(preq);
		break;

	default:
		BUG();
	}

	if (release_ioc) {
		struct io_context * ioc = current->io_context;
		current->io_context = saved_ioc;
#ifdef CONFIG_BEANCOUNTERS
		set_exec_ub(saved_ub);
#endif
		put_io_context(ioc);
	}
}

static void ploop_wait(struct ploop_device * plo, int once, struct blk_plug *plug)
{
	DEFINE_WAIT(_wait);
	for (;;) {
		prepare_to_wait(&plo->waitq, &_wait, TASK_INTERRUPTIBLE);

		/* This is obvious. */
		if (!list_empty(&plo->ready_queue))
			break;

		/* This is not. If we have something in entry queue... */
		if (!list_empty(&plo->entry_queue)) {
			/* And entry queue is not suspended due to barrier
			 * or active reuests are all completed, so that
			 * we can start/finish barrier processing
			 */
			if (!once &&
			    (!test_bit(PLOOP_S_ATTENTION, &plo->state) ||
			     !plo->active_reqs))
				break;
		} else if (plo->bio_head ||
			   (!bio_list_empty(&plo->bio_discard_list) &&
			    !ploop_discard_is_inprogress(plo->fbd))) {
			/* ready_queue and entry_queue are empty, but
			 * bio list not. Obviously, we'd like to process
			 * bio_list instead of sleeping */
			if (!list_empty(&plo->free_list) &&
			    (!test_bit(PLOOP_S_ATTENTION, &plo->state) ||
			     !plo->active_reqs))
				break;
		}

		if (kthread_should_stop() && !plo->active_reqs)
			break;

		set_bit(PLOOP_S_WAIT_PROCESS, &plo->state);
		if (kthread_should_stop())
			set_bit(PLOOP_S_EXITING, &plo->state);
		once = 0;
		spin_unlock_irq(&plo->lock);
		blk_finish_plug(plug);
		schedule();
		blk_start_plug(plug);
		spin_lock_irq(&plo->lock);
		clear_bit(PLOOP_S_WAIT_PROCESS, &plo->state);
	}
	finish_wait(&plo->waitq, &_wait);
}

static void ploop_handle_enospc_req(struct ploop_request *preq)
{
	struct ploop_device * plo = preq->plo;
	DEFINE_WAIT(_wait);

	if (test_bit(PLOOP_S_ABORT, &plo->state))
		return;

	mod_timer(&plo->freeze_timer, jiffies + HZ * 10);

	prepare_to_wait(&plo->freeze_waitq, &_wait, TASK_INTERRUPTIBLE);
	spin_unlock_irq(&plo->lock);
	schedule();
	spin_lock_irq(&plo->lock);

	finish_wait(&plo->freeze_waitq, &_wait);

	spin_unlock_irq(&plo->lock);
	if (preq->aux_bio) {
		int i;
		struct bio * bio = preq->aux_bio;

		for (i = 0; i < bio->bi_vcnt; i++)
			put_page(bio->bi_io_vec[i].bv_page);

		bio_put(bio);

		preq->aux_bio = NULL;
	}
	spin_lock_irq(&plo->lock);

	del_lockout(preq);

	if (!list_empty(&preq->delay_list))
		list_splice_init(&preq->delay_list, plo->ready_queue.prev);

	if (preq->map) {
		map_release(preq->map);
		preq->map = NULL;
	}
	if (preq->trans_map) {
		map_release(preq->trans_map);
		preq->trans_map = NULL;
	}

	preq->eng_state = PLOOP_E_ENTRY;
	preq->error = 0;
	preq->tstamp = jiffies;
	preq->iblock = 0;
}

/* Main process. Processing queues in proper order, handling pre-barrier
 * flushes and queue suspend while processing a barrier
 */
static int ploop_thread(void * data)
{
	int once = 0;
	struct ploop_device * plo = data;
	struct blk_plug plug;
	LIST_HEAD(drop_list);

	set_user_nice(current, -20);

	blk_start_plug(&plug);
	spin_lock_irq(&plo->lock);
	for (;;) {
		/* Convert bios to preqs early (at least before processing
		 * entry queue) to increase chances of bio merge
		 */
	again:
		BUG_ON (!list_empty(&drop_list));

		process_bio_queue(plo, &drop_list);
		process_discard_bio_queue(plo, &drop_list);

		if (!list_empty(&drop_list)) {
			spin_unlock_irq(&plo->lock);
			ploop_preq_drop(plo, &drop_list, 1);
			goto again;
		}

		if (!list_empty(&plo->ready_queue)) {
			struct ploop_request * preq;
			preq = ploop_get_request(plo, &plo->ready_queue);
			if (preq->error == -ENOSPC)
				ploop_handle_enospc_req(preq);
			spin_unlock_irq(&plo->lock);

			ploop_req_state_process(preq);

			spin_lock_irq(&plo->lock);
			continue;
		}

		/* Now ready_queue is empty */

		if (plo->active_reqs == 0)
			clear_bit(PLOOP_S_ATTENTION, &plo->state);

		if (!list_empty(&plo->entry_queue) &&
		    !test_bit(PLOOP_S_ATTENTION, &plo->state)) {
			struct ploop_request * preq;

			preq = ploop_get_request(plo, &plo->entry_queue);

			if (test_bit(PLOOP_REQ_BARRIER, &preq->state)) {
				set_bit(PLOOP_S_ATTENTION, &plo->state);
				if (plo->active_reqs) {
					list_add(&preq->list, &plo->entry_queue);
					continue;
				}
				plo->barrier_reqs--;
			} else {
				if (!plo->read_sync_reqs &&
				    plo->active_reqs > plo->tune.max_active_requests &&
				    plo->active_reqs > plo->entry_qlen &&
				    time_before(jiffies, preq->tstamp + plo->tune.batch_entry_delay) &&
				    !kthread_should_stop()) {
					list_add(&preq->list, &plo->entry_queue);
					once = 1;
					mod_timer(&plo->mitigation_timer, preq->tstamp + plo->tune.batch_entry_delay);
					goto wait_more;
				}
			}

			plo->active_reqs++;
			ploop_entry_qlen_dec(preq);

			if (test_bit(PLOOP_REQ_DISCARD, &preq->state)) {
				BUG_ON(plo->maintenance_type != PLOOP_MNTN_DISCARD);
				atomic_inc(&plo->maintenance_cnt);
			}

			if (test_bit(PLOOP_REQ_SORTED, &preq->state)) {
				rb_erase(&preq->lockout_link, &plo->entry_tree[preq->req_rw & WRITE]);
				__clear_bit(PLOOP_REQ_SORTED, &preq->state);
			}
			preq->eng_state = PLOOP_E_ENTRY;
			spin_unlock_irq(&plo->lock);

			ploop_req_state_process(preq);

			spin_lock_irq(&plo->lock);
			continue;
		}

		/* Termination condition: stop requested,
		 * no requests are in process or in entry queue
		 */
		if (kthread_should_stop() && !plo->active_reqs &&
		    list_empty(&plo->entry_queue) && !plo->bio_head &&
		    bio_list_empty(&plo->bio_discard_list))
			break;

wait_more:
		ploop_wait(plo, once, &plug);
		once = 0;
	}
	spin_unlock_irq(&plo->lock);
	blk_finish_plug(&plug);

	return 0;
}


/* block device operations */
static int ploop_open(struct block_device *bdev, fmode_t fmode)
{
	struct ploop_device * plo = bdev->bd_disk->private_data;

	mutex_lock(&plo->ctl_mutex);

	BUG_ON (plo->bdev && plo->bdev != bdev);
	if (!plo->bdev)
		plo->bdev = bdev;

	atomic_inc(&plo->open_count);
	mutex_unlock(&plo->ctl_mutex);

	check_disk_change(bdev);

	return 0;
}

static void ploop_release(struct gendisk *disk, fmode_t fmode)
{
	struct ploop_device *plo = disk->private_data;

	mutex_lock(&plo->ctl_mutex);
	if (atomic_dec_and_test(&plo->open_count)) {
		ploop_tracker_stop(plo, 1);
		plo->bdev = NULL;
	}
	mutex_unlock(&plo->ctl_mutex);
}

static struct ploop_delta *
init_delta(struct ploop_device * plo, struct ploop_ctl * ctl, int level)
{
	struct ploop_delta * delta;
	struct ploop_delta_ops * ops;
	int err;

	ops = ploop_format_get(ctl->pctl_format);
	if (ops == NULL)
		return ERR_PTR(-EINVAL);

	if (level < 0 && !list_empty(&plo->map.delta_list)) {
		struct ploop_delta * top_delta = ploop_top_delta(plo);
		err = -EINVAL;
		if (top_delta->level >= 127)
			goto out_err;
		level = top_delta->level + 1;
		if (ctl->pctl_cluster_log != plo->cluster_log)
			goto out_err;
		if (!(ops->capability & PLOOP_FMT_CAP_DELTA))
			goto out_err;
	} else if (level >= 0) {
		struct ploop_delta * delta = find_delta(plo, level);
		err = -EINVAL;
		if (delta == NULL)
			goto out_err;
		if (ctl->pctl_cluster_log != plo->cluster_log)
			goto out_err;
		if (level && !(ops->capability & PLOOP_FMT_CAP_DELTA))
			goto out_err;
	}

	if (level < 0)
		level = 0;

	err = -ENOMEM;
	delta = kzalloc(sizeof(struct ploop_delta), GFP_KERNEL);
	if (delta == NULL)
		goto out_err;

	__module_get(THIS_MODULE);

	delta->level = level;
	delta->cluster_log = ctl->pctl_cluster_log;
	delta->plo = plo;
	delta->ops = ops;
	delta->flags = ctl->pctl_flags & PLOOP_FMT_FLAGS;
	delta->max_delta_size = ULLONG_MAX;

	KOBJECT_INIT(&delta->kobj, &ploop_delta_ktype);
	return delta;

out_err:
	ploop_format_put(ops);
	return ERR_PTR(err);
}


static int ploop_set_max_delta_size(struct ploop_device *plo, unsigned long arg)
{
	struct ploop_delta * top_delta = ploop_top_delta(plo);
	u64 max_delta_size;

	if (copy_from_user(&max_delta_size, (void*)arg, sizeof(u64)))
		return -EFAULT;

	if (top_delta == NULL)
		return -EINVAL;

	top_delta->max_delta_size = max_delta_size;

	return 0;
}

static int ploop_add_delta(struct ploop_device * plo, unsigned long arg)
{
	int err;
	struct ploop_ctl ctl;
	struct ploop_ctl_chunk chunk;
	struct ploop_delta * delta;

	if (copy_from_user(&ctl, (void*)arg, sizeof(struct ploop_ctl)))
		return -EFAULT;
	if (ctl.pctl_chunks != 1)
		return -EINVAL;
	if (copy_from_user(&chunk, (void*)arg + sizeof(struct ploop_ctl),
			   sizeof(struct ploop_ctl_chunk)))
		return -EFAULT;

	if ((ctl.pctl_flags & PLOOP_FLAG_COOKIE) && !plo->cookie[0] &&
	    copy_from_user(plo->cookie, (void*)arg + sizeof(struct ploop_ctl) +
			   sizeof(struct ploop_ctl_chunk),
			   PLOOP_COOKIE_SIZE - 1))
		return -EFAULT;

	if (test_bit(PLOOP_S_RUNNING, &plo->state))
		return -EBUSY;
	if (plo->maintenance_type != PLOOP_MNTN_OFF)
		return -EBUSY;

	delta = init_delta(plo, &ctl, -1);
	if (IS_ERR(delta))
		return PTR_ERR(delta);

	err = delta->ops->compose(delta, 1, &chunk);
	if (err)
		goto out_destroy;

	if (list_empty(&plo->map.delta_list))
		plo->fmt_version = PLOOP_FMT_UNDEFINED;

	err = delta->ops->open(delta);
	if (err)
		goto out_destroy;

	if (list_empty(&plo->map.delta_list)) {
		plo->cluster_log = delta->cluster_log;
	} else {
		struct ploop_delta * top_delta = ploop_top_delta(plo);

		err = -EINVAL;
		if (!(top_delta->flags & PLOOP_FMT_RDONLY))
			goto out_close;
	}

	err = KOBJECT_ADD(&delta->kobj, kobject_get(&plo->kobj),
			  "%d", delta->level);
	if (err < 0) {
		kobject_put(&plo->kobj);
		goto out_close;
	}

	mutex_lock(&plo->sysfs_mutex);
	list_add(&delta->list, &plo->map.delta_list);
	mutex_unlock(&plo->sysfs_mutex);
	set_bit(PLOOP_S_CHANGED, &plo->state);

	return 0;

out_close:
	delta->ops->stop(delta);
out_destroy:
	delta->ops->destroy(delta);
	kobject_put(&delta->kobj);
	return err;
}

static int ploop_replace_delta(struct ploop_device * plo, unsigned long arg)
{
	int err;
	struct ploop_ctl ctl;
	struct ploop_ctl_chunk chunk;
	struct ploop_delta * delta, * old_delta;

	if (copy_from_user(&ctl, (void*)arg, sizeof(struct ploop_ctl)))
		return -EFAULT;
	if (ctl.pctl_chunks != 1)
		return -EINVAL;
	if (copy_from_user(&chunk, (void*)arg + sizeof(struct ploop_ctl),
			   sizeof(struct ploop_ctl_chunk)))
		return -EFAULT;

	if (plo->maintenance_type != PLOOP_MNTN_OFF)
		return -EBUSY;

	old_delta = find_delta(plo, ctl.pctl_level);
	if (old_delta == NULL)
		return -ENOENT;

	if ((old_delta->flags ^ ctl.pctl_flags) & PLOOP_FMT_RDONLY)
		return -EINVAL;

	delta = init_delta(plo, &ctl, ctl.pctl_level);
	if (IS_ERR(delta))
		return PTR_ERR(delta);

	err = delta->ops->compose(delta, 1, &chunk);
	if (err)
		goto out_destroy;

	err = delta->ops->open(delta);
	if (err)
		goto out_destroy;

	kobject_del(&old_delta->kobj);

	err = KOBJECT_ADD(&delta->kobj, kobject_get(&plo->kobj),
			  "%d", delta->level);
	kobject_put(&plo->kobj);

	if (err < 0) {
		kobject_put(&plo->kobj);
		goto out_close;
	}

	ploop_quiesce(plo);
	ploop_map_destroy(&plo->map);
	list_replace_init(&old_delta->list, &delta->list);
	ploop_relax(plo);

	old_delta->ops->stop(old_delta);
	old_delta->ops->destroy(old_delta);
	kobject_put(&old_delta->kobj);
	return 0;

out_close:
	delta->ops->stop(delta);
out_destroy:
	delta->ops->destroy(delta);
	kobject_put(&delta->kobj);
	return err;
}


void ploop_quiesce(struct ploop_device * plo)
{
	struct completion qcomp;
	struct ploop_request * preq;

	if (!test_bit(PLOOP_S_RUNNING, &plo->state))
		return;

	spin_lock_irq(&plo->lock);
	preq = ploop_alloc_request(plo);
	preq->bl.head = preq->bl.tail = NULL;
	preq->req_size = 0;
	preq->req_rw = 0;
	preq->eng_state = PLOOP_E_ENTRY;
	preq->state = (1 << PLOOP_REQ_SYNC) | (1 << PLOOP_REQ_BARRIER);
	preq->error = 0;
	preq->tstamp = jiffies;

	init_completion(&qcomp);
	init_completion(&plo->relax_comp);
	init_completion(&plo->relaxed_comp);
	plo->quiesce_comp = &qcomp;

	ploop_entry_add(plo, preq);
	plo->barrier_reqs++;

	if (test_bit(PLOOP_S_WAIT_PROCESS, &plo->state))
		wake_up_interruptible(&plo->waitq);
	spin_unlock_irq(&plo->lock);

	wait_for_completion(&qcomp);
	plo->quiesce_comp = NULL;
}

void ploop_relax(struct ploop_device * plo)
{
	if (!test_bit(PLOOP_S_RUNNING, &plo->state))
		return;

	complete(&plo->relax_comp);
	wait_for_completion(&plo->relaxed_comp);
}

/* search disk for first partition bdev with mounted fs and freeze it */
static struct super_block *find_and_freeze_bdev(struct gendisk *disk,
						struct block_device ** bdev_pp)
{
	struct super_block  * sb   = NULL;
	struct block_device * bdev = NULL;
	int i;

	for (i = 0; i <= (*bdev_pp)->bd_part_count; i++) {
		bdev = bdget_disk(disk, i);
		if (!bdev)
			break;

		sb = freeze_bdev(bdev);
		if (sb)
			break;

		thaw_bdev(bdev, sb);
		bdput(bdev);
		bdev = NULL;
	}

	*bdev_pp = bdev;
	return sb;
}

static int ploop_snapshot(struct ploop_device * plo, unsigned long arg,
			  struct block_device * bdev)
{
	int err;
	struct ploop_ctl ctl;
	struct ploop_ctl_chunk chunk;
	struct ploop_delta * delta, * top_delta;
	struct ploop_snapdata snapdata;
	struct super_block * sb;

	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	if (!test_bit(PLOOP_S_RUNNING, &plo->state))
		return ploop_add_delta(plo, arg);
	if (plo->maintenance_type != PLOOP_MNTN_OFF)
		return -EBUSY;

	if (copy_from_user(&ctl, (void*)arg, sizeof(struct ploop_ctl)))
		return -EFAULT;
	if (ctl.pctl_chunks != 1)
		return -EINVAL;
	if (copy_from_user(&chunk, (void*)arg + sizeof(struct ploop_ctl),
			   sizeof(struct ploop_ctl_chunk)))
		return -EFAULT;

	delta = init_delta(plo, &ctl, -1);
	if (IS_ERR(delta))
		return PTR_ERR(delta);

	err = delta->ops->compose(delta, 1, &chunk);
	if (err)
		goto out_destroy;

	err = delta->ops->open(delta);
	if (err)
		goto out_destroy;

	err = KOBJECT_ADD(&delta->kobj, kobject_get(&plo->kobj),
			  "%d", delta->level);
	if (err)
		goto out_close;

	top_delta = ploop_top_delta(plo);

	err = top_delta->ops->prepare_snapshot(top_delta, &snapdata);
	if (err)
		goto out_close2;

	/* _XXX_ only one mounted fs per ploop-device is supported */
	sb = NULL;
	if (ctl.pctl_flags & PLOOP_FLAG_FS_SYNC) {
		/* freeze_bdev() may trigger ploop_bd_full() */
		plo->maintenance_type = PLOOP_MNTN_SNAPSHOT;
		mutex_unlock(&plo->ctl_mutex);
		sb = find_and_freeze_bdev(plo->disk, &bdev);
		mutex_lock(&plo->ctl_mutex);
		plo->maintenance_type = PLOOP_MNTN_OFF;
		if (IS_ERR(sb)) {
			err = PTR_ERR(sb);
			fput(snapdata.file);
			goto out_close2;
		}
	}

	ploop_quiesce(plo);
	err = top_delta->ops->complete_snapshot(top_delta, &snapdata);
	if (!err) {
		mutex_lock(&plo->sysfs_mutex);
		list_add(&delta->list, &plo->map.delta_list);
		clear_bit(PLOOP_MAP_IDENTICAL, &plo->map.flags);
		mutex_unlock(&plo->sysfs_mutex);
	}
	ploop_relax(plo);

	if ((ctl.pctl_flags & PLOOP_FLAG_FS_SYNC) && bdev) {
		/* Drop ctl_mutex in order to avoid reverse order locking
		   thaw_bdev() ->kill_sb() ->blkdev_put() ->bd_mutex */
		plo->maintenance_type = PLOOP_MNTN_SNAPSHOT;
		mutex_unlock(&plo->ctl_mutex);
		thaw_bdev(bdev, sb);
		mutex_lock(&plo->ctl_mutex);
		plo->maintenance_type = PLOOP_MNTN_OFF;
		bdput(bdev);
	}

	if (err)
		goto out_close2;

	return 0;

out_close2:
	kobject_del(&delta->kobj);
out_close:
	kobject_put(&plo->kobj);
	delta->ops->stop(delta);
out_destroy:
	delta->ops->destroy(delta);
	kobject_put(&delta->kobj);
	return err;
}

static void renumber_deltas(struct ploop_device * plo)
{
	struct ploop_delta * delta;
	int level = 0;

	list_for_each_entry_reverse(delta, &plo->map.delta_list, list) {
		delta->level = level++;
	}

	if (level == 1) {
		delta = ploop_top_delta(plo);
		if (delta->level == 0 &&
		    (delta->ops->capability & PLOOP_FMT_CAP_IDENTICAL))
			set_bit(PLOOP_MAP_IDENTICAL, &plo->map.flags);
	}
}

static void rename_deltas(struct ploop_device * plo, int level)
{
	struct ploop_delta * delta;

	list_for_each_entry_reverse(delta, &plo->map.delta_list, list) {
		int err;

		if (delta->level < level)
			continue;
#if 0
		/* Oops, kobject_rename() is not exported! */
		sprintf(nname, "%d", delta->level);
		err = kobject_rename(&delta->kobj, nname);
#else
		kobject_del(&delta->kobj);
		err = KOBJECT_ADD(&delta->kobj, &plo->kobj,
				  "%d", delta->level);
#endif
		if (err)
			printk("rename_deltas: %d %d %d\n", err, level, delta->level);
	}
}

/* Delete delta. Obviously, removing an arbitrary delta will destroy
 * all the data unless this delta is empty or its data are completely
 * covered by higher delta or lower delta contains the whole copy of delta,
 * which is deleted. Driver does not check this.
 *
 * Some cases, f.e. removing writable top delta are never valid,
 * because caller has no way to ensure that new data do not emerge.
 * Nevertheless, we do _NOT_ prohibit this operation, assuming
 * that caller have some knowledge, which we cannot comprehend.
 * F.e. virtual machine using the device was stopped, device
 * was synced and data were copied to lower delta. And this is bad
 * idea. This should be different ioctl.
 */

static int ploop_del_delta(struct ploop_device * plo, unsigned long arg)
{
	__u32 level;
	struct ploop_delta * delta, * next;

	if (copy_from_user(&level, (void*)arg, 4))
		return -EFAULT;

	if (plo->maintenance_type != PLOOP_MNTN_OFF)
		return -EBUSY;

	if (level == 0 && test_bit(PLOOP_S_RUNNING, &plo->state)) {
		printk(KERN_INFO "Can't del base delta on running ploop%d\n",
		       plo->index);
		return -EBUSY;
	}

	delta = find_delta(plo, level);

	if (delta == NULL)
		return -ENOENT;

	kobject_del(&delta->kobj);
	kobject_put(&plo->kobj);

	ploop_quiesce(plo);
	next = list_entry(delta->list.next, struct ploop_delta, list);
	list_del(&delta->list);
	if (list_empty(&plo->map.delta_list))
		plo->cookie[0] = 0;
	if (level != 0)
		next->ops->refresh(next);
	if (test_bit(PLOOP_S_RUNNING, &plo->state))
		ploop_map_remove_delta(&plo->map, level);
	renumber_deltas(plo);
	ploop_relax(plo);
	rename_deltas(plo, level);

	delta->ops->stop(delta);
	delta->ops->destroy(delta);
	kobject_put(&delta->kobj);
	BUG_ON(test_bit(PLOOP_S_RUNNING, &plo->state) &&
	       list_empty(&plo->map.delta_list));
	return 0;
}

static void ploop_merge_process(struct ploop_device * plo)
{
	int num_reqs;

	spin_lock_irq(&plo->lock);

	atomic_set(&plo->maintenance_cnt, 1);
	plo->merge_ptr = 0;

	init_completion(&plo->maintenance_comp);

	num_reqs = plo->tune.fsync_max;
	if (num_reqs > plo->tune.max_requests/2)
		num_reqs = plo->tune.max_requests/2;
	if (num_reqs < 1)
		num_reqs = 1;

	for (; num_reqs; num_reqs--) {
		struct ploop_request * preq;

		preq = ploop_alloc_request(plo);

		preq->bl.tail = preq->bl.head = NULL;
		preq->req_cluster = ~0U;
		preq->req_size = 0;
		preq->req_rw = WRITE_SYNC;
		preq->eng_state = PLOOP_E_ENTRY;
		preq->state = (1 << PLOOP_REQ_SYNC) | (1 << PLOOP_REQ_MERGE);
		preq->error = 0;
		preq->tstamp = jiffies;
		preq->iblock = 0;
		preq->prealloc_size = 0;

		atomic_inc(&plo->maintenance_cnt);

		ploop_entry_add(plo, preq);

		if (test_bit(PLOOP_S_WAIT_PROCESS, &plo->state))
			wake_up_interruptible(&plo->waitq);
	}

	if (atomic_dec_and_test(&plo->maintenance_cnt))
		complete(&plo->maintenance_comp);

	spin_unlock_irq(&plo->lock);
}

int ploop_maintenance_wait(struct ploop_device * plo)
{
	int err;

	mutex_unlock(&plo->ctl_mutex);

	err = wait_for_completion_interruptible(&plo->maintenance_comp);

	mutex_lock(&plo->ctl_mutex);

	return atomic_read(&plo->maintenance_cnt) ? err : 0;
}

static void ploop_update_fmt_version(struct ploop_device * plo)
{
	struct ploop_delta * delta = ploop_top_delta(plo);

	if (delta->level == 0 &&
	    (delta->ops->capability & PLOOP_FMT_CAP_IDENTICAL)) {
		ploop_map_destroy(&plo->map);
		set_bit(PLOOP_MAP_IDENTICAL, &plo->map.flags);
		plo->fmt_version = PLOOP_FMT_UNDEFINED;
	}
}

static void ploop_merge_cleanup(struct ploop_device * plo,
				struct ploop_map * map,
				struct ploop_delta * delta, int err)
{
	ploop_quiesce(plo);
	mutex_lock(&plo->sysfs_mutex);
	list_del(&delta->list);

	if (err)
		list_add(&delta->list, &plo->map.delta_list);
	else
		ploop_update_fmt_version(plo);

	plo->trans_map = NULL;
	plo->maintenance_type = PLOOP_MNTN_OFF;
	mutex_unlock(&plo->sysfs_mutex);
	ploop_map_destroy(map);
	ploop_relax(plo);
}

static int ploop_merge(struct ploop_device * plo)
{
	int err;
	struct ploop_map * map;
	struct ploop_delta * delta, * next;
	struct ploop_snapdata sd;

	if (plo->maintenance_type == PLOOP_MNTN_MERGE)
		goto already;

	if (plo->maintenance_type != PLOOP_MNTN_OFF)
		return -EBUSY;

	BUG_ON (plo->trans_map);

	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	delta = ploop_top_delta(plo);
	if (delta->level == 0)
		return -ENOENT;

	map = kzalloc(sizeof(struct ploop_map), GFP_KERNEL);
	if (map == NULL)
		return -ENOMEM;

	map_init(plo, map);
	ploop_map_start(map, plo->bd_size);

	next = list_entry(delta->list.next, struct ploop_delta, list);

	err = next->ops->prepare_merge(next, &sd);
	if (err) {
		printk(KERN_WARNING "prepare_merge for ploop%d failed (%d)\n",
		       plo->index, err);
		goto out;
	}

	ploop_quiesce(plo);

	if (test_bit(PLOOP_S_RUNNING, &plo->state))
		ploop_map_destroy(&plo->map);

	err = next->ops->start_merge(next, &sd);

	if (!err) {
		mutex_lock(&plo->sysfs_mutex);
		list_del(&delta->list);
		list_add(&delta->list, &map->delta_list);
		delta->level = 0;
		plo->trans_map = map;
		plo->maintenance_type = PLOOP_MNTN_MERGE;
		mutex_unlock(&plo->sysfs_mutex);
	} else {
		/* Yes. All transient obstacles must be resolved
		 * in prepare_merge. Failed start_merge means
		 * abort of the device.
		 */
		printk(KERN_WARNING "start_merge for ploop%d failed (%d)\n",
		       plo->index, err);
		set_bit(PLOOP_S_ABORT, &plo->state);
	}

	ploop_relax(plo);

	if (err)
		goto out;

	ploop_merge_process(plo);

already:
	err = ploop_maintenance_wait(plo);
	if (err)
		return err;

	BUG_ON(atomic_read(&plo->maintenance_cnt));

	if (plo->maintenance_type != PLOOP_MNTN_MERGE)
		return -EALREADY;

	map = plo->trans_map;
	BUG_ON (!map);

	delta = map_top_delta(plo->trans_map);

	if (test_bit(PLOOP_S_ABORT, &plo->state)) {
		printk(KERN_WARNING "merge for ploop%d failed (state ABORT)\n",
		       plo->index);
		err = -EIO;
	}

	ploop_merge_cleanup(plo, map, delta, err);

	if (!err) {
		kobject_del(&delta->kobj);
		kobject_put(&plo->kobj);

		delta->ops->stop(delta);
		delta->ops->destroy(delta);
		kobject_put(&delta->kobj);
	}
out:
	kfree(map);
	return err;
}

static int ploop_truncate(struct ploop_device * plo, unsigned long arg)
{
	int err;
	struct ploop_truncate_ctl ctl;
	struct ploop_delta * delta;
	struct file * file;

	if (copy_from_user(&ctl, (void*)arg, sizeof(struct ploop_truncate_ctl)))
		return -EFAULT;

	if (ctl.fd < 0)
		return -EBADF;

	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	delta = find_delta(plo, ctl.level);
	if (delta == NULL)
		return -ENOENT;

	if (!(delta->flags & PLOOP_FMT_RDONLY))
		return -EBUSY;

	if (delta->ops->truncate == NULL)
		return -EOPNOTSUPP;

	file = fget(ctl.fd);
	if (file == NULL)
		return -EBADF;

	ploop_quiesce(plo);

	ploop_map_destroy(&plo->map);

	err = delta->ops->truncate(delta, file, ctl.alloc_head);
	if (!err)
		delta->io.prealloced_size = 0;

	ploop_relax(plo);

	fput(file);

	return err;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
static int ploop_issue_flush_fn(request_queue_t *q, struct gendisk *disk,
				sector_t *error_sector)
{
	/* Should queue barrier request on ploop and wait for completion.
	 * The interface is broken and obsolete.
	 */
	return -EOPNOTSUPP;
}
#endif

#define FUSE_SUPER_MAGIC 0x65735546
#define IS_PSTORAGE(sb) (sb->s_magic == FUSE_SUPER_MAGIC && \
			 !strcmp(sb->s_subtype, "pstorage"))

static int ploop_bd_full(struct backing_dev_info *bdi, long long nr, int root)
{
	struct ploop_device *plo      = bdi->congested_data;
	u64		     reserved = 0;
	int		     rc	      = 0;

	if (root) {
		if (!plo->tune.disable_root_threshold)
			reserved = (u64)root_threshold * 1024;
	} else {
		if (!plo->tune.disable_user_threshold)
			reserved = (u64)user_threshold * 1024;
	}

	if (reserved) {
		struct kstatfs buf;
		int	       ret;

		struct ploop_delta *top_delta;
		struct file	   *file;
		struct super_block *sb;
		void		   *jctx = current->journal_info;

		mutex_lock(&plo->sysfs_mutex);
		top_delta = ploop_top_delta(plo);
		file	  = top_delta->io.files.file;
		sb	  = F_DENTRY(file)->d_inode->i_sb;

		/* bd_full can be unsupported or not needed */
		if (IS_PSTORAGE(sb) || sb->s_op->statfs == simple_statfs ||
		    top_delta->flags & PLOOP_FMT_PREALLOCATED) {
			mutex_unlock(&plo->sysfs_mutex);
			return 0;
		}

		get_file(file);
		mutex_unlock(&plo->sysfs_mutex);

		current->journal_info = NULL;
		ret = sb->s_op->statfs(F_DENTRY(file), &buf);
		if (ret || buf.f_bfree * buf.f_bsize < reserved + nr) {
			static unsigned long full_warn_time;

			if (printk_timed_ratelimit(&full_warn_time, 60*60*HZ))
				printk(KERN_WARNING
				       "ploop%d: host disk is almost full "
				       "(%llu < %llu); CT sees -ENOSPC !\n",
				       plo->index, buf.f_bfree * buf.f_bsize,
				       reserved + nr);

			rc = 1;
		}

		fput(file);
		current->journal_info = jctx;
	}

	return rc;
}

static int ploop_start(struct ploop_device * plo, struct block_device *bdev)
{
	int err;
	struct ploop_delta * top_delta, * delta;
	int i;

	if (test_bit(PLOOP_S_RUNNING, &plo->state))
		return -EBUSY;

	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	for (i = 0; i < plo->tune.max_requests; i++) {
		struct ploop_request * preq;
		preq = kzalloc(sizeof(struct ploop_request), GFP_KERNEL);
		if (preq == NULL)
			break;

		preq->plo = plo;
		INIT_LIST_HEAD(&preq->delay_list);
		list_add(&preq->list, &plo->free_list);
	}

	list_for_each_entry_reverse(delta, &plo->map.delta_list, list) {
		err = delta->ops->start(delta);
		if (err)
			return err;
	}

	ploop_map_start(&plo->map, plo->bd_size);

	top_delta = ploop_top_delta(plo);

	if (top_delta->level == 0 &&
	    (top_delta->ops->capability & PLOOP_FMT_CAP_IDENTICAL))
		set_bit(PLOOP_MAP_IDENTICAL, &plo->map.flags);

	/* Deltas are ready. Enable block device. */
	set_device_ro(bdev, (top_delta->flags & PLOOP_FMT_RDONLY) != 0);

	blk_queue_make_request(plo->queue, ploop_make_request);
	plo->queue->queuedata = plo;
	plo->queue->backing_dev_info.congested_fn = ploop_congested;
	plo->queue->backing_dev_info.congested_fn2 = ploop_congested2;
	plo->queue->backing_dev_info.bd_full_fn = ploop_bd_full;
	plo->queue->backing_dev_info.congested_data = plo;

	blk_queue_merge_bvec(plo->queue, ploop_merge_bvec);
	blk_queue_flush(plo->queue, REQ_FLUSH);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
	blk_queue_issue_flush_fn(plo->queue, ploop_issue_flush_fn);
#endif

	if (top_delta->io.ops->queue_settings)
		top_delta->io.ops->queue_settings(&top_delta->io, plo->queue);

	blk_queue_max_discard_sectors(plo->queue, INT_MAX);
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, plo->queue);

	set_capacity(plo->disk, plo->bd_size);
	bd_set_size(bdev, (loff_t)plo->bd_size << 9);
	set_blocksize(bdev, PAGE_SIZE);

	plo->thread = kthread_create(ploop_thread, plo, "ploop%d",
				     plo->index);
	if (IS_ERR(plo->thread)) {
		err = PTR_ERR(plo->thread);
		goto out_err;
	}

	wake_up_process(plo->thread);
	set_bit(PLOOP_S_RUNNING, &plo->state);
	BUG_ON(list_empty(&plo->map.delta_list));
	return 0;

out_err:
	plo->thread = NULL;
	set_capacity(plo->disk, 0);
	bd_set_size(bdev, 0);
	return err;
}

static int ploop_stop(struct ploop_device * plo, struct block_device *bdev)
{
	int p;
	struct ploop_delta * delta;
	int cnt;

	if (bdev != bdev->bd_contains) {
		if (printk_ratelimit())
			printk(KERN_INFO "stop ploop%d failed (wrong bdev)\n",
			       plo->index);
		return -ENODEV;
	}

	if (bdev->bd_contains->bd_holders) {
		if (printk_ratelimit())
			printk(KERN_INFO "stop ploop%d failed (holders=%d)\n",
			       plo->index, bdev->bd_contains->bd_holders);
		return -EBUSY;
	}

	if (!test_bit(PLOOP_S_RUNNING, &plo->state))
		return -EINVAL;

	if (list_empty(&plo->map.delta_list)) {
		printk(KERN_INFO "stop ploop%d failed (no deltas)\n",
		       plo->index);
		return -ENOENT;
	}

	cnt = atomic_read(&plo->open_count);
	if (cnt > 1) {
		if (printk_ratelimit())
			printk(KERN_INFO "stop ploop%d failed (cnt=%d)\n",
			       plo->index, cnt);
		return -EBUSY;
	}

	cnt = atomic_read(&plo->maintenance_cnt);
	if (plo->maintenance_type != PLOOP_MNTN_OFF && cnt) {
		if (printk_ratelimit())
			printk(KERN_INFO "stop ploop%d failed "
			       "(type=%d cnt=%d)\n",
			       plo->index, plo->maintenance_type, cnt);
		return -EBUSY;
	}

	for (p = plo->disk->minors - 1; p > 0; p--)
		invalidate_partition(plo->disk, p);
	invalidate_partition(plo->disk, 0);

	clear_bit(PLOOP_S_RUNNING, &plo->state);

	del_timer_sync(&plo->mitigation_timer);
	del_timer_sync(&plo->freeze_timer);

	/* This will wait for queue drain */
	kthread_stop(plo->thread);
	plo->thread = NULL;

	/* queue drained, no more ENOSPC */
	spin_lock_irq(&plo->lock);
	if (waitqueue_active(&plo->event_waitq))
		wake_up_interruptible(&plo->event_waitq);
	spin_unlock_irq(&plo->lock);

	BUG_ON(plo->entry_qlen);
	BUG_ON(plo->active_reqs);
	BUG_ON(plo->barrier_reqs);
	BUG_ON(plo->fastpath_reqs);
	BUG_ON(plo->read_sync_reqs);

	list_for_each_entry(delta, &plo->map.delta_list, list) {
		delta->ops->stop(delta);
	}

	set_capacity(plo->disk, 0);
	bd_set_size(bdev, 0);

	if (plo->cached_bio) {
		bio_put(plo->cached_bio);
		plo->cached_bio = NULL;
	}

	while (!list_empty(&plo->free_list)) {
		struct ploop_request * preq;

		preq = list_first_entry(&plo->free_list, struct ploop_request, list);
		list_del(&preq->list);
		kfree(preq);
	}

	ploop_map_destroy(&plo->map);
	if (plo->trans_map)
		ploop_map_destroy(plo->trans_map);

	return 0;
}

static int ploop_sync(struct ploop_device * plo, struct block_device *bdev)
{
	struct ploop_delta * delta;

	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	delta = ploop_top_delta(plo);

	if (delta->ops->sync == NULL)
		return 0;

	return delta->ops->sync(delta);
}

static void destroy_deltas(struct ploop_device * plo, struct ploop_map * map)
{
	while (!list_empty(&map->delta_list)) {
		struct ploop_delta * delta;
		delta = list_entry(map->delta_list.next, struct ploop_delta, list);

		mutex_lock(&plo->sysfs_mutex);
		list_del(&delta->list);
		mutex_unlock(&plo->sysfs_mutex);

		kobject_del(&delta->kobj);
		kobject_put(&plo->kobj);

		delta->ops->destroy(delta);
		kobject_put(&delta->kobj);
	}

	plo->cookie[0] = 0;
}

static int ploop_clear(struct ploop_device * plo, struct block_device * bdev)
{
	int cnt;

	if (test_bit(PLOOP_S_RUNNING, &plo->state)) {
		if (printk_ratelimit())
			printk(KERN_INFO "clear ploop%d failed (RUNNING)\n",
			       plo->index);
		return -EBUSY;
	}
	if (plo->maintenance_type == PLOOP_MNTN_TRACK) {
		if (printk_ratelimit())
			printk(KERN_INFO "clear ploop%d failed (TRACK)\n",
			       plo->index);
		return -EBUSY;
	}
	cnt = atomic_read(&plo->maintenance_cnt);
	if (plo->maintenance_type != PLOOP_MNTN_OFF && cnt) {
		if (printk_ratelimit())
			printk(KERN_INFO "clear ploop%d failed "
			       "(type=%d cnt=%d)\n",
			       plo->index, plo->maintenance_type, cnt);
		return -EBUSY;
	}

	clear_bit(PLOOP_S_DISCARD_LOADED, &plo->state);
	clear_bit(PLOOP_S_DISCARD, &plo->state);

	destroy_deltas(plo, &plo->map);

	if (plo->trans_map) {
		struct ploop_map * map;
		destroy_deltas(plo, plo->trans_map);
		map = plo->trans_map;
		plo->trans_map = NULL;
		kfree(map);
	}

	ploop_fb_fini(plo->fbd, 0);

	plo->maintenance_type = PLOOP_MNTN_OFF;
	plo->bd_size = 0;
	plo->state = (1 << PLOOP_S_CHANGED);
	BUG_ON(test_bit(PLOOP_S_RUNNING, &plo->state));
	return 0;
}

static int ploop_index_update_ioc(struct ploop_device *plo, unsigned long arg)
{
	struct ploop_index_update_ctl ctl;
	struct reloc_map *map;
	int i;

	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	if (copy_from_user(&ctl, (void*)arg,
			   sizeof(struct ploop_index_update_ctl)))
		return -EFAULT;

	if (!ctl.n_maps)
		return 0;

	map = kzalloc(sizeof(*map) * ctl.n_maps, GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	if (copy_from_user(map, (u8*)arg + sizeof(ctl),
			   sizeof(*map) * ctl.n_maps)) {
		kfree(map);
		return -EFAULT;
	}

	ploop_quiesce(plo);

	for (i = 0; i < ctl.n_maps; i++)
		ploop_update_map(&plo->map, ctl.level,
				 map[i].req_cluster, map[i].iblk);

	ploop_relax(plo);

	kfree(map);
	return 0;
}

static void ploop_relocate(struct ploop_device * plo)
{
	struct ploop_request * preq;

	spin_lock_irq(&plo->lock);

	atomic_set(&plo->maintenance_cnt, 1);
	plo->grow_relocated = 0;

	init_completion(&plo->maintenance_comp);

	preq = ploop_alloc_request(plo);

	preq->bl.tail = preq->bl.head = NULL;
	preq->req_cluster = 0;
	preq->req_size = 0;
	preq->req_rw = WRITE_SYNC;
	preq->eng_state = PLOOP_E_ENTRY;
	preq->state = (1 << PLOOP_REQ_SYNC) | (1 << PLOOP_REQ_RELOC_A);
	preq->error = 0;
	preq->tstamp = jiffies;
	preq->iblock = 0;
	preq->prealloc_size = 0;

	atomic_inc(&plo->maintenance_cnt);

	ploop_entry_add(plo, preq);

	if (test_bit(PLOOP_S_WAIT_PROCESS, &plo->state))
		wake_up_interruptible(&plo->waitq);

	if (atomic_dec_and_test(&plo->maintenance_cnt))
		complete(&plo->maintenance_comp);

	spin_unlock_irq(&plo->lock);
}

static int ploop_grow(struct ploop_device *plo, struct block_device *bdev,
		      unsigned long arg)
{
	u64 new_size;
	struct ploop_ctl ctl;
	struct ploop_delta *delta = ploop_top_delta(plo);
	int reloc = 0; /* 'relocation needed' flag */
	int err;

	if (!delta)
		return -ENOENT;

	if (plo->maintenance_type == PLOOP_MNTN_GROW)
		goto already;

	if (plo->maintenance_type != PLOOP_MNTN_OFF)
		return -EBUSY;

	if (copy_from_user(&ctl, (void*)arg, sizeof(struct ploop_ctl)))
		return -EFAULT;

	if (ctl.pctl_cluster_log != plo->cluster_log)
		return -EINVAL;

	if (ctl.pctl_flags & PLOOP_FLAG_CLUBLKS)
		new_size = (u64)ctl.pctl_size << plo->cluster_log;
	else
		new_size = ctl.pctl_size;

	if (plo->bd_size > new_size) /* online shrink not supported */
		return -EINVAL;

	if (plo->bd_size == new_size) /* nothing to do */
		return 0;

	if (!delta->ops->prepare_grow)
		return -EINVAL;

	ploop_quiesce(plo);
	err = delta->ops->prepare_grow(delta, &new_size, &reloc);
	if (err)
		goto grow_failed;

	plo->grow_new_size = new_size;

	/* prepare_grow() succeeded, but more actions needed */
	if (reloc) {
		plo->maintenance_type = PLOOP_MNTN_GROW;
		ploop_relax(plo);
		ploop_relocate(plo);
already:
		err = ploop_maintenance_wait(plo);
		if (err)
			return err;

		BUG_ON(atomic_read(&plo->maintenance_cnt));

		if (plo->maintenance_type != PLOOP_MNTN_GROW)
			return -EALREADY;

		if (test_bit(PLOOP_S_ABORT, &plo->state)) {
			plo->maintenance_type = PLOOP_MNTN_OFF;
			return -EIO;
		}

		ploop_quiesce(plo);
		new_size = plo->grow_new_size;
		plo->maintenance_type = PLOOP_MNTN_OFF;
	}

	/* Update bdev size and friends */
	if (delta->ops->complete_grow) {
		err = delta->ops->complete_grow(delta, new_size);
		if (err)
			goto grow_failed;
	}

	mutex_lock(&plo->sysfs_mutex);
	plo->bd_size = new_size;
	plo->map.max_index = (plo->bd_size + (1 << plo->cluster_log) - 1 )
			     >> plo->cluster_log;

	set_capacity(plo->disk, plo->bd_size);
	bd_set_size(bdev, (loff_t)plo->bd_size << 9);

	mutex_unlock(&plo->sysfs_mutex);
grow_failed:
	ploop_relax(plo);
	return err;
}

static int ploop_balloon_ioc(struct ploop_device *plo, unsigned long arg)
{
	struct ploop_balloon_ctl ctl;
	struct ploop_delta *delta = ploop_top_delta(plo);

	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	if (copy_from_user(&ctl, (void*)arg, sizeof(ctl)))
		return -EFAULT;

	if (ctl.inflate && ctl.keep_intact)
		return -EINVAL;

	switch (plo->maintenance_type) {
	case PLOOP_MNTN_DISCARD:
		if (!test_bit(PLOOP_S_DISCARD_LOADED, &plo->state))
			break;

		ploop_quiesce(plo);
		clear_bit(PLOOP_S_DISCARD_LOADED, &plo->state);
		plo->maintenance_type = PLOOP_MNTN_FBLOADED;
		ploop_fb_lost_range_init(plo->fbd, delta->io.alloc_head);
		ploop_relax(plo);
		/* fall through */
	case PLOOP_MNTN_FBLOADED:
	case PLOOP_MNTN_RELOC:
		BUG_ON (!plo->fbd);
		ctl.alloc_head = ploop_fb_get_alloc_head(plo->fbd);
		ctl.level      = ploop_fb_get_freezed_level(plo->fbd);
		break;
	case PLOOP_MNTN_OFF:
		if (ctl.inflate) {
			if (delta->ops->id != PLOOP_FMT_PLOOP1)
				return -EOPNOTSUPP;

			ploop_quiesce(plo);
			plo->maintenance_type = PLOOP_MNTN_BALLOON;
			ploop_relax(plo);
		}
		break;
	case PLOOP_MNTN_BALLOON :
		if (!ctl.inflate && !ctl.keep_intact) {
			ploop_quiesce(plo);
			plo->maintenance_type = PLOOP_MNTN_OFF;
			ploop_relax(plo);
		}
	}
	ctl.mntn_type = plo->maintenance_type;

	return copy_to_user((void*)arg, &ctl, sizeof(ctl));
}

static int ploop_freeblks_ioc(struct ploop_device *plo, unsigned long arg)
{
	struct ploop_delta *delta;
	struct ploop_freeblks_ctl ctl;
	struct ploop_freeblks_ctl_extent *extents;
	struct ploop_freeblks_desc *fbd;
	int i;
	int rc = 0;

	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	if (plo->maintenance_type == PLOOP_MNTN_OFF)
		return -EINVAL;
	if (plo->maintenance_type != PLOOP_MNTN_BALLOON)
		return -EBUSY;
	BUG_ON (plo->fbd);

	if (copy_from_user(&ctl, (void*)arg, sizeof(ctl)))
		return -EFAULT;

	extents = kzalloc(sizeof(*extents) * ctl.n_extents, GFP_KERNEL);
	if (!extents)
		return -ENOMEM;

	delta = ploop_top_delta(plo);
	if (delta->level != ctl.level) {
		rc = -EINVAL;
		goto free_extents;
	}

	if (copy_from_user(extents, (u8*)arg + sizeof(ctl),
			   sizeof(*extents) * ctl.n_extents)) {
		rc = -EINVAL;
		goto free_extents;
	}

	fbd = ploop_fb_init(plo);
	if (!fbd) {
		rc = -ENOMEM;
		goto free_extents;
	}

	for (i = 0; i < ctl.n_extents; i++) {
		rc = ploop_fb_add_free_extent(fbd, extents[i].clu,
					      extents[i].iblk, extents[i].len);
		if (rc) {
			ploop_fb_fini(fbd, rc);
			goto free_extents;
		}
	}

	ploop_quiesce(plo);

	ctl.alloc_head = delta->io.alloc_head;
	if (copy_to_user((void*)arg, &ctl, sizeof(ctl))) {
		rc = -EFAULT;
		ploop_fb_fini(fbd, rc);
	} else {
		iblock_t a_h = delta->io.alloc_head;
		/* make fbd visible to ploop engine */
		plo->fbd = fbd;
		plo->maintenance_type = PLOOP_MNTN_FBLOADED;
		BUG_ON (a_h != ctl.alloc_head); /* quiesce sanity */
		ploop_fb_lost_range_init(fbd, a_h);
		ploop_fb_set_freezed_level(fbd, delta->level);
	}

	ploop_relax(plo);
free_extents:
	kfree(extents);
	return rc;
}

static int ploop_fbget_ioc(struct ploop_device *plo, unsigned long arg)
{
	struct ploop_freeblks_ctl ctl;
	int rc = 0;

	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	if (plo->maintenance_type == PLOOP_MNTN_DISCARD) {
		if (!test_bit(PLOOP_S_DISCARD_LOADED, &plo->state))
			return -EINVAL;
	} else if (plo->maintenance_type != PLOOP_MNTN_FBLOADED)
		return -EINVAL;
	BUG_ON (!plo->fbd);

	if (copy_from_user(&ctl, (void*)arg, sizeof(ctl)))
		return -EFAULT;

	ploop_quiesce(plo);
	rc = ploop_fb_copy_freeblks_to_user(plo->fbd, (void*)arg, &ctl);
	ploop_relax(plo);

	return rc;
}

static int ploop_fbfilter_ioc(struct ploop_device *plo, unsigned long arg)
{
	int rc = 0;

	if (plo->maintenance_type != PLOOP_MNTN_DISCARD ||
	    !test_bit(PLOOP_S_DISCARD_LOADED, &plo->state))
		return -EINVAL;

	BUG_ON (!plo->fbd);

	ploop_quiesce(plo);
	rc = ploop_fb_filter_freeblks(plo->fbd, arg);
	ploop_relax(plo);

	return rc;
}

static void ploop_relocblks_process(struct ploop_device *plo)
{
	int num_reqs;
	struct ploop_request *preq;

	num_reqs = plo->tune.fsync_max;
	if (num_reqs > plo->tune.max_requests/2)
		num_reqs = plo->tune.max_requests/2;
	if (num_reqs < 1)
		num_reqs = 1;

	spin_lock_irq(&plo->lock);

	atomic_set(&plo->maintenance_cnt, 1);

	init_completion(&plo->maintenance_comp);

	for (; num_reqs; num_reqs--) {
		preq = ploop_alloc_request(plo);

		preq->bl.tail = preq->bl.head = NULL;
		preq->req_cluster = ~0U; /* uninitialized */
		preq->req_size = 0;
		preq->req_rw = WRITE_SYNC;
		preq->eng_state = PLOOP_E_ENTRY;
		preq->state = (1 << PLOOP_REQ_SYNC) | (1 << PLOOP_REQ_RELOC_S);
		preq->error = 0;
		preq->tstamp = jiffies;
		preq->iblock = 0;
		preq->prealloc_size = 0;

		atomic_inc(&plo->maintenance_cnt);

		ploop_entry_add(plo, preq);

		if (test_bit(PLOOP_S_WAIT_PROCESS, &plo->state))
			wake_up_interruptible(&plo->waitq);
	}

	if (atomic_dec_and_test(&plo->maintenance_cnt))
		complete(&plo->maintenance_comp);

	spin_unlock_irq(&plo->lock);
}

static int release_fbd(struct ploop_device *plo,
		       struct ploop_relocblks_ctl_extent *e,
		       int err)
{
	if (e)
		kfree(e);

	clear_bit(PLOOP_S_DISCARD, &plo->state);

	ploop_quiesce(plo);
	ploop_fb_fini(plo->fbd, err);
	plo->maintenance_type = PLOOP_MNTN_OFF;
	ploop_relax(plo);

	return err;
}

static void ploop_discard_restart(struct ploop_device *plo, int err)
{
	if (!err && test_bit(PLOOP_S_DISCARD, &plo->state)) {
		ploop_fb_reinit(plo->fbd, 0);
		atomic_set(&plo->maintenance_cnt, 0);
		init_completion(&plo->maintenance_comp);
		plo->maintenance_type = PLOOP_MNTN_DISCARD;
	} else {
		clear_bit(PLOOP_S_DISCARD, &plo->state);
		ploop_fb_fini(plo->fbd, err);
		plo->maintenance_type = PLOOP_MNTN_OFF;
	}
}

static int ploop_fbdrop_ioc(struct ploop_device *plo)
{
	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	if (plo->maintenance_type == PLOOP_MNTN_DISCARD) {
		if (!test_bit(PLOOP_S_DISCARD_LOADED, &plo->state))
			return -EINVAL;
	} else if (plo->maintenance_type != PLOOP_MNTN_FBLOADED)
		return -EINVAL;
	BUG_ON (!plo->fbd);

	ploop_quiesce(plo);
	ploop_discard_restart(plo, 0);
	ploop_relax(plo);

	return 0;
}

static int ploop_relocblks_ioc(struct ploop_device *plo, unsigned long arg)
{
	struct ploop_delta *delta = ploop_top_delta(plo);
	struct ploop_relocblks_ctl ctl;
	struct ploop_relocblks_ctl_extent *extents = NULL;
	struct ploop_freeblks_desc *fbd = plo->fbd;
	int i;
	int err = 0;
	int n_free;

	if (list_empty(&plo->map.delta_list))
		return -ENOENT;

	if (!fbd || (plo->maintenance_type != PLOOP_MNTN_FBLOADED &&
		     plo->maintenance_type != PLOOP_MNTN_RELOC))
		return -EINVAL;

	BUG_ON(test_bit(PLOOP_S_DISCARD_LOADED, &plo->state));

	if (copy_from_user(&ctl, (void*)arg, sizeof(ctl)))
		return -EFAULT;

	if (delta->level != ctl.level ||
	    ploop_fb_get_freezed_level(plo->fbd) != ctl.level ||
	    ploop_fb_get_alloc_head(plo->fbd) != ctl.alloc_head) {
		return -EINVAL;
	}

	if (plo->maintenance_type == PLOOP_MNTN_RELOC)
		goto already;

	if (ctl.n_extents) {
		extents = kzalloc(sizeof(*extents) * ctl.n_extents,
				GFP_KERNEL);
		if (!extents)
			return release_fbd(plo, extents, -ENOMEM);

		if (copy_from_user(extents, (u8*)arg + sizeof(ctl),
					sizeof(*extents) * ctl.n_extents))
			return release_fbd(plo, extents, -EINVAL);

		for (i = 0; i < ctl.n_extents; i++) {
			err = ploop_fb_add_reloc_extent(fbd, extents[i].clu,
					extents[i].iblk,
					extents[i].len,
					extents[i].free);
			if (err)
				return release_fbd(plo, extents, err);
		}

		kfree(extents);
		extents = NULL;
	}

	ploop_quiesce(plo);

	/* alloc_head must never decrease */
	BUG_ON (delta->io.alloc_head < ploop_fb_get_alloc_head(plo->fbd));
	n_free = ploop_fb_get_n_free(plo->fbd);

	/*
	 * before relocation start, freeblks engine could provide only
	 * free blocks
	 */
	BUG_ON (delta->io.alloc_head > ploop_fb_get_alloc_head(plo->fbd) &&
		n_free);
	ploop_fb_relocation_start(plo->fbd, ctl.n_scanned);

	if (!n_free || !ctl.n_extents)
		goto truncate;

	plo->maintenance_type = PLOOP_MNTN_RELOC;

	ploop_relax(plo);

	ploop_relocblks_process(plo);
already:
	err = ploop_maintenance_wait(plo);
	if (err)
		return err;

	BUG_ON(atomic_read(&plo->maintenance_cnt));

	if (plo->maintenance_type != PLOOP_MNTN_RELOC)
		return -EALREADY;

	fbd = plo->fbd;
	BUG_ON (!fbd);

	if (test_bit(PLOOP_S_ABORT, &plo->state)) {
		clear_bit(PLOOP_S_DISCARD,&plo->state);

		ploop_fb_fini(plo->fbd, -EIO);
		plo->maintenance_type = PLOOP_MNTN_OFF;
		return -EIO;
	}

	if (ploop_fb_get_n_relocated(fbd) != ploop_fb_get_n_relocating(fbd))
		return release_fbd(plo, extents, -EIO);

	/* time to truncate */
	ploop_quiesce(plo);
truncate:
	if (ploop_fb_get_lost_range_len(plo->fbd) != 0) {
		BUG_ON (delta->io.alloc_head >
			ploop_fb_get_alloc_head(plo->fbd));
		err = delta->ops->truncate(delta, NULL,
					   ploop_fb_get_first_lost_iblk(plo->fbd));
		if (!err) {
			delta->io.prealloced_size = 0;
			ctl.alloc_head = ploop_fb_get_lost_range_len(plo->fbd);
			err = copy_to_user((void*)arg, &ctl, sizeof(ctl));
		}
	} else {
		ctl.alloc_head = 0;
		err = copy_to_user((void*)arg, &ctl, sizeof(ctl));
	}

	ploop_discard_restart(plo, err);

	ploop_relax(plo);
	return err;
}

static int ploop_getdevice_ioc(unsigned long arg)
{
	int err;
	int index = 0;
	struct rb_node *n;
	struct ploop_getdevice_ctl ctl = {};

	mutex_lock(&ploop_devices_mutex);
	for (n = rb_first(&ploop_devices_tree); n; n = rb_next(n), index++) {
		struct ploop_device *plo;
		plo = rb_entry(n, struct ploop_device, link);
		if (plo->index != index || list_empty(&plo->map.delta_list))
			break;
	}
	mutex_unlock(&ploop_devices_mutex);

	ctl.minor = index << PLOOP_PART_SHIFT;
	if (ctl.minor & ~MINORMASK)
		return -ERANGE;
	err = copy_to_user((void*)arg, &ctl, sizeof(ctl));
	return err;
}

static int ploop_ioctl(struct block_device *bdev, fmode_t fmode, unsigned int cmd,
		       unsigned long arg)
{
	struct ploop_device *plo = bdev->bd_disk->private_data;
	int err = -EINVAL;

	if (!ve_is_super(get_exec_env()))
		return -EPERM;

	mutex_lock(&plo->ctl_mutex);

	if (plo->maintenance_type == PLOOP_MNTN_SNAPSHOT) {
		mutex_unlock(&plo->ctl_mutex);
		return -EBUSY;
	}

	switch(cmd) {
	case PLOOP_IOC_ADD_DELTA:
		err = ploop_add_delta(plo, arg);
		break;
	case PLOOP_IOC_DEL_DELTA:
		err = ploop_del_delta(plo, arg);
		break;
	case PLOOP_IOC_REPLACE_DELTA:
		err = ploop_replace_delta(plo, arg);
		break;
	case PLOOP_IOC_SNAPSHOT:
		err = ploop_snapshot(plo, arg, bdev);
		break;
	case PLOOP_IOC_CLEAR:
		err = ploop_clear(plo, bdev);
		break;
	case PLOOP_IOC_STOP:
		err = ploop_stop(plo, bdev);
		break;
	case PLOOP_IOC_START:
		err = ploop_start(plo, bdev);
		break;
	case PLOOP_IOC_SYNC:
		err = ploop_sync(plo, bdev);
		break;

	case PLOOP_IOC_TRACK_INIT:
		err = ploop_tracker_init(plo, arg);
		break;
	case PLOOP_IOC_TRACK_SETPOS:
		err = ploop_tracker_setpos(plo, arg);
		break;
	case PLOOP_IOC_TRACK_STOP:
		err = ploop_tracker_stop(plo, 0);
		break;
	case PLOOP_IOC_TRACK_ABORT:
		err = ploop_tracker_stop(plo, 1);
		break;
	case PLOOP_IOC_TRACK_READ:
		err = ploop_tracker_read(plo, arg);
		break;

	case PLOOP_IOC_MERGE:
		err = ploop_merge(plo);
		break;
	case PLOOP_IOC_TRUNCATE:
		err = ploop_truncate(plo, arg);
		break;
	case PLOOP_IOC_UPDATE_INDEX:
		err = ploop_index_update_ioc(plo, arg);
		break;
	case PLOOP_IOC_GROW:
		err = ploop_grow(plo, bdev, arg);
		break;
	case PLOOP_IOC_BALLOON:
		err = ploop_balloon_ioc(plo, arg);
		break;
	case PLOOP_IOC_FREEBLKS:
		err = ploop_freeblks_ioc(plo, arg);
		break;
	case PLOOP_IOC_FBGET:
		err = ploop_fbget_ioc(plo, arg);
		break;
	case PLOOP_IOC_FBFILTER:
		err = ploop_fbfilter_ioc(plo, arg);
		break;
	case PLOOP_IOC_FBDROP:
		err = ploop_fbdrop_ioc(plo);
		break;
	case PLOOP_IOC_RELOCBLKS:
		err = ploop_relocblks_ioc(plo, arg);
		break;
	case PLOOP_IOC_GETDEVICE:
		err = ploop_getdevice_ioc(arg);
		break;

	case PLOOP_IOC_DISCARD_INIT:
		err = ploop_discard_init_ioc(plo);
		break;
	case PLOOP_IOC_DISCARD_FINI:
		err = ploop_discard_fini_ioc(plo);
		break;
	case PLOOP_IOC_DISCARD_WAIT:
		err = ploop_discard_wait_ioc(plo);
		break;
	case PLOOP_IOC_MAX_DELTA_SIZE:
		err = ploop_set_max_delta_size(plo, arg);
		break;
	default:
		err = -EINVAL;
	}
	mutex_unlock(&plo->ctl_mutex);
	return err;
}

static int ploop_media_changed(struct gendisk *disk)
{
	struct ploop_device *plo = disk->private_data;

	return test_bit(PLOOP_S_CHANGED, &plo->state);
}

static int ploop_revalidate(struct gendisk *disk)
{
	struct ploop_device *plo = disk->private_data;

	clear_bit(PLOOP_S_CHANGED, &plo->state);
	return 0;
}

static struct block_device_operations ploop_dev_fops = {
	.owner =		THIS_MODULE,
	.open =			ploop_open,
	.release =		ploop_release,
	.ioctl =		ploop_ioctl,
	.media_changed =	ploop_media_changed,
	.revalidate_disk =	ploop_revalidate,
};

MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(PLOOP_DEVICE_MAJOR);

atomic_t plo_count = ATOMIC_INIT(0);

static struct sysfs_ops ploop_sysfs_ops = { };

static void ploop_obj_release(struct kobject *kobj)
{
	struct ploop_device *plo = container_of(kobj, struct ploop_device, kobj);
	kfree(plo);
	atomic_dec(&plo_count);
}

static struct kobj_type ploop_ktype = {
	.sysfs_ops	= &ploop_sysfs_ops,
	.release	= ploop_obj_release,
};

static struct ploop_device *__ploop_dev_alloc(int index)
{
	struct ploop_device *plo;
	struct gendisk *dk;

	plo = kzalloc(sizeof(*plo), GFP_KERNEL);
	if(!plo)
		goto out;

	plo->queue = blk_alloc_queue(GFP_KERNEL);
	if (!plo->queue)
		goto out_mem;

	dk = plo->disk = alloc_disk(PLOOP_PART_MAX);
	if (!plo->disk)
		goto out_queue;

	spin_lock_init(&plo->lock);
	spin_lock_init(&plo->dummy_lock);
	plo->queue->queue_lock = &plo->dummy_lock;
	mutex_init(&plo->ctl_mutex);
	mutex_init(&plo->sysfs_mutex);
	plo->index = index;
	plo->state = 0;
	atomic_set(&plo->open_count, 0);
	init_timer(&plo->mitigation_timer);
	plo->mitigation_timer.function = mitigation_timeout;
	plo->mitigation_timer.data = (unsigned long)plo;
	init_timer(&plo->freeze_timer);
	plo->freeze_timer.function = freeze_timeout;
	plo->freeze_timer.data = (unsigned long)plo;
	INIT_LIST_HEAD(&plo->entry_queue);
	plo->entry_tree[0] = plo->entry_tree[1] = RB_ROOT;
	plo->lockout_tree = RB_ROOT;
	INIT_LIST_HEAD(&plo->ready_queue);
	INIT_LIST_HEAD(&plo->free_list);
	init_waitqueue_head(&plo->waitq);
	init_waitqueue_head(&plo->req_waitq);
	init_waitqueue_head(&plo->freeze_waitq);
	init_waitqueue_head(&plo->event_waitq);
	plo->tune = DEFAULT_PLOOP_TUNE;
	map_init(plo, &plo->map);
	track_init(plo);
	KOBJECT_INIT(&plo->kobj, &ploop_ktype);
	atomic_inc(&plo_count);
	bio_list_init(&plo->bio_discard_list);

	dk->major		= ploop_major;
	dk->first_minor		= index << PLOOP_PART_SHIFT;
	dk->minors		= PLOOP_PART_MAX;
	dk->fops		= &ploop_dev_fops;
	dk->private_data	= plo;
	dk->queue		= plo->queue;
	snprintf(dk->disk_name, sizeof(dk->disk_name), "ploop%d", index);
	return plo;

out_queue:
	blk_cleanup_queue(plo->queue);
out_mem:
	kfree(plo);
out:
	return NULL;
}

static void ploop_dev_del(struct ploop_device *plo)
{
	ploop_tracker_destroy(plo, 1);
	ploop_sysfs_uninit(plo);
	del_gendisk(plo->disk);
	blk_cleanup_queue(plo->queue);
	put_disk(plo->disk);
	rb_erase(&plo->link, &ploop_devices_tree);
	ploop_fb_fini(plo->fbd, 0);
	kobject_put(&plo->kobj);
}

static void ploop_dev_insert(struct ploop_device *plo)
{
	struct rb_node ** p;
	struct rb_node *parent = NULL;
	struct ploop_device * pl;

	p = &ploop_devices_tree.rb_node;
	while (*p) {
		parent = *p;
		pl = rb_entry(parent, struct ploop_device, link);
		BUG_ON (plo->index == pl->index);

		if (plo->index < pl->index)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	rb_link_node(&plo->link, parent, p);
	rb_insert_color(&plo->link, &ploop_devices_tree);
}

static struct ploop_device *ploop_dev_search(int index)
{
	struct rb_node *n = ploop_devices_tree.rb_node;

	while(n) {
		struct ploop_device *plo;
		plo = rb_entry(n, struct ploop_device, link);

		if (index < plo->index)
			n = n->rb_left;
		else if (index > plo->index)
			n = n->rb_right;
		else
			return plo;
	}

	return NULL;
}

static struct ploop_device *ploop_dev_init(int index)
{
	struct ploop_device *plo = ploop_dev_search(index);

	if (plo)
		return plo;

	plo = __ploop_dev_alloc(index);
	if (plo) {
		add_disk(plo->disk);
		ploop_sysfs_init(plo);
		ploop_dev_insert(plo);
	}
	return plo;
}

static struct kobject *ploop_dev_probe(dev_t dev, int *part, void *data)
{
	struct kobject *kobj;
	struct ploop_device *plo;

	*part = dev & (PLOOP_PART_MAX - 1);
	mutex_lock(&ploop_devices_mutex);
	plo = ploop_dev_init((dev & MINORMASK) >> PLOOP_PART_SHIFT);
	if (!plo)
		kobj = ERR_PTR(-ENOMEM);
	else
		kobj = get_disk(plo->disk);
	mutex_unlock(&ploop_devices_mutex);

	return kobj;
}

/* Functions to service /proc/vz/ploop_minor */

static int ploop_minor_show(struct seq_file *m, void *v)
{
	struct ploop_device *plo = m->private;
	seq_printf(m, "%d\n", plo->index << PLOOP_PART_SHIFT);
	return 0;
}

/* Returns random index from 10000 - 65535 range */
static unsigned ploop_random_index(void)
{
	unsigned int n;

	get_random_bytes(&n, sizeof(n));

	return 10000 + n % (65536 - 10000);
}

static int ploop_minor_open(struct inode *inode, struct file *file)
{
	int index = 0;
	struct rb_node *n;
	struct ploop_device *plo = NULL;
	int found = 0;
	int ret;

	mutex_lock(&ploop_devices_mutex);
	for (n = rb_first(&ploop_devices_tree); n; n = rb_next(n)) {
		plo = rb_entry(n, struct ploop_device, link);
		if (list_empty(&plo->map.delta_list) &&
		    !test_bit(PLOOP_S_LOCKED, &plo->locking_state)) {
			found = 1;
			break;
		}
	}

	if (!found) {
		int i = 0;

		index = ploop_random_index();
		plo = ploop_dev_search(index);

		while (plo) {
			for (n = &plo->link; n; n = rb_next(n), index++) {
				plo = rb_entry(n, struct ploop_device, link);
				if (plo->index != index ||
				    (list_empty(&plo->map.delta_list) &&
				     !test_bit(PLOOP_S_LOCKED, &plo->locking_state)))
					break;
			}

			BUG_ON (plo->index == index);

			/* not more than two iterations */
			if (i++ == 2)
				break;

			if ((index << PLOOP_PART_SHIFT) & ~MINORMASK) {
				index = 0;
				plo = ploop_dev_search(index);
			} else
				plo = NULL;
		}
		
		if ((index << PLOOP_PART_SHIFT) & ~MINORMASK) {
			mutex_unlock(&ploop_devices_mutex);
			return -ERANGE;
		}

		plo = __ploop_dev_alloc(index);
		if (!plo) {
			mutex_unlock(&ploop_devices_mutex);
			return -ENOMEM;
		}

		add_disk(plo->disk);
		ploop_sysfs_init(plo);
		ploop_dev_insert(plo);
	}
	set_bit(PLOOP_S_LOCKED, &plo->locking_state);
	mutex_unlock(&ploop_devices_mutex);

	ret = single_open(file, ploop_minor_show, plo);
	if (ret)
		clear_bit(PLOOP_S_LOCKED, &plo->locking_state);
	return ret;
}

static int ploop_minor_release(struct inode *inode, struct file *filp)
{
	struct ploop_device *plo = ((struct seq_file *)filp->private_data)->private;
	clear_bit(PLOOP_S_LOCKED, &plo->locking_state);
	return single_release(inode, filp);
}

static const struct file_operations proc_ploop_minor = {
	.owner          = THIS_MODULE,
	.open		= ploop_minor_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= ploop_minor_release,
};

module_param(ploop_max, int, 0);
MODULE_PARM_DESC(ploop_max, "Maximum number of ploop devices");
module_param(ploop_major, int, 0);
MODULE_PARM_DESC(ploop_major, "Major number of ploop device");
module_param(max_map_pages, int, 0644);
MODULE_PARM_DESC(ploop_max_map_pages, "Maximal amount of pages taken by map cache");
module_param(root_threshold, long, 0644);
MODULE_PARM_DESC(root_threshold, "Disk space reserved for root (in kilobytes)");
module_param(user_threshold, long, 0644);
MODULE_PARM_DESC(user_threshold, "Disk space reserved for user (in kilobytes)");
module_param(large_disk_support, int, 0444);
MODULE_PARM_DESC(ploop_large_disk_support, "Support of large disks (>2TB)");

static int __init ploop_mod_init(void)
{
	int err;

	/* _XXX_ should be estimated from available ram */
	if (max_map_pages == 0)
		max_map_pages = 1024;

	err = ploop_map_init();
	if (err)
		goto out_err;

	if (register_blkdev(ploop_major, "ploop"))
		goto out_err;

	blk_register_region(MKDEV(ploop_major, 0), ploop_max,
			THIS_MODULE, ploop_dev_probe, NULL, NULL);

	if (!proc_create("ploop_minor", 0440,
			 proc_vz_dir, &proc_ploop_minor))
		goto out_err2;

	printk(KERN_INFO "ploop_dev: module loaded\n");
	return 0;

out_err2:
	err = -ENOMEM;
	blk_unregister_region(MKDEV(ploop_major, 0), ploop_max);
	unregister_blkdev(PLOOP_DEVICE_MAJOR, "ploop");
out_err:
	ploop_map_exit();
	return err;
}

static void __exit ploop_mod_exit(void)
{
	struct rb_node * n;

	remove_proc_entry("ploop_minor", proc_vz_dir);
	while ((n = rb_first(&ploop_devices_tree)) != NULL)
		ploop_dev_del(rb_entry(n, struct ploop_device, link));
	blk_unregister_region(MKDEV(ploop_major, 0), ploop_max);
	unregister_blkdev(PLOOP_DEVICE_MAJOR, "ploop");
	ploop_map_exit();
	WARN_ON(atomic_read(&plo_count));
}
module_init(ploop_mod_init);
module_exit(ploop_mod_exit);
