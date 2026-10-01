// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/page_io.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, 
 *  Asynchronous swapping added 30.12.95. Stephen Tweedie
 *  Removed race in async swapping. 14.4.1996. Bruno Haible
 *  Add swap of shared pages through the page cache. 20.2.1998. Stephen Tweedie
 *  Always use brw_page, life becomes simpler. 12 May 1998 Eric Biederman
 */

#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/swapops.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/frontswap.h>
#include <linux/blkdev.h>
#include <linux/psi.h>
#include <linux/uio.h>
#include <linux/sched/task.h>
#ifdef CONFIG_ZRAM_KCOMPRESSD
#include <linux/kthread.h>
#include <linux/freezer.h>
#endif
#include <asm/pgtable.h>

#ifdef CONFIG_ZRAM_KCOMPRESSD
int sysctl_kcompressd __read_mostly = 1;

static bool kcompress_queue_empty(struct pglist_data *pgdat)
{
	return READ_ONCE(pgdat->kcompress_head) ==
		READ_ONCE(pgdat->kcompress_tail);
}

static bool kcompress_enqueue(struct pglist_data *pgdat, struct bio *bio)
{
	unsigned long flags;
	bool queued = false;

	spin_lock_irqsave(&pgdat->kcompress_lock, flags);
	if (pgdat->kcompress_head - pgdat->kcompress_tail <
	    KCOMPRESS_QUEUE_SIZE) {
		pgdat->kcompress_queue[pgdat->kcompress_head &
					KCOMPRESS_QUEUE_MASK] = bio;
		pgdat->kcompress_head++;
		queued = true;
	}
	spin_unlock_irqrestore(&pgdat->kcompress_lock, flags);

	return queued;
}

static struct bio *kcompress_dequeue(struct pglist_data *pgdat)
{
	unsigned long flags;
	struct bio *bio = NULL;

	spin_lock_irqsave(&pgdat->kcompress_lock, flags);
	if (pgdat->kcompress_head != pgdat->kcompress_tail) {
		bio = pgdat->kcompress_queue[pgdat->kcompress_tail &
					     KCOMPRESS_QUEUE_MASK];
		pgdat->kcompress_tail++;
	}
	spin_unlock_irqrestore(&pgdat->kcompress_lock, flags);

	return bio;
}
#endif

static struct bio *get_swap_bio(gfp_t gfp_flags,
				struct page *page, bio_end_io_t end_io)
{
	struct bio *bio;

	bio = bio_alloc(gfp_flags, 1);
	if (bio) {
		struct block_device *bdev;

		bio->bi_iter.bi_sector = map_swap_page(page, &bdev);
		bio_set_dev(bio, bdev);
		bio->bi_iter.bi_sector <<= PAGE_SHIFT - 9;
		bio->bi_end_io = end_io;

		bio_add_page(bio, page, PAGE_SIZE * hpage_nr_pages(page), 0);
	}
	return bio;
}

void end_swap_bio_write(struct bio *bio)
{
	struct page *page = bio_first_page_all(bio);

	if (bio->bi_status) {
		SetPageError(page);
		/*
		 * We failed to write the page out to swap-space.
		 * Re-dirty the page in order to avoid it being reclaimed.
		 * Also print a dire warning that things will go BAD (tm)
		 * very quickly.
		 *
		 * Also clear PG_reclaim to avoid rotate_reclaimable_page()
		 */
		set_page_dirty(page);
		pr_alert_ratelimited("Write-error on swap-device (%u:%u:%llu)\n",
			 MAJOR(bio_dev(bio)),
			 MINOR(bio_dev(bio)),
			 (unsigned long long)bio->bi_iter.bi_sector);
		ClearPageReclaim(page);
	}
	end_page_writeback(page);
	bio_put(bio);
}

static void end_swap_bio_read(struct bio *bio)
{
	struct page *page = bio_first_page_all(bio);
	struct task_struct *waiter = bio->bi_private;

	if (bio->bi_status) {
		SetPageError(page);
		ClearPageUptodate(page);
		pr_alert("Read-error on swap-device (%u:%u:%llu)\n",
			 MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
			 (unsigned long long)bio->bi_iter.bi_sector);
		goto out;
	}

	SetPageUptodate(page);
out:
	unlock_page(page);
	WRITE_ONCE(bio->bi_private, NULL);
	bio_put(bio);
	if (waiter) {
		blk_wake_io_task(waiter);
		put_task_struct(waiter);
	}
}

int generic_swapfile_activate(struct swap_info_struct *sis,
				struct file *swap_file,
				sector_t *span)
{
	struct address_space *mapping = swap_file->f_mapping;
	struct inode *inode = mapping->host;
	unsigned blocks_per_page;
	unsigned long page_no;
	unsigned blkbits;
	sector_t probe_block;
	sector_t last_block;
	sector_t lowest_block = -1;
	sector_t highest_block = 0;
	int nr_extents = 0;
	int ret;

	blkbits = inode->i_blkbits;
	blocks_per_page = PAGE_SIZE >> blkbits;

	/*
	 * Map all the blocks into the extent tree.  This code doesn't try
	 * to be very smart.
	 */
	probe_block = 0;
	page_no = 0;
	last_block = i_size_read(inode) >> blkbits;
	while ((probe_block + blocks_per_page) <= last_block &&
			page_no < sis->max) {
		unsigned block_in_page;
		sector_t first_block;

		cond_resched();

		first_block = probe_block;
		ret = bmap(inode, &first_block);
		if (ret || !first_block)
			goto bad_bmap;

		/*
		 * It must be PAGE_SIZE aligned on-disk
		 */
		if (first_block & (blocks_per_page - 1)) {
			probe_block++;
			goto reprobe;
		}

		for (block_in_page = 1; block_in_page < blocks_per_page;
					block_in_page++) {
			sector_t block;

			block = probe_block + block_in_page;
			ret = bmap(inode, &block);
			if (ret || !block)
				goto bad_bmap;

			if (block != first_block + block_in_page) {
				/* Discontiguity */
				probe_block++;
				goto reprobe;
			}
		}

		first_block >>= (PAGE_SHIFT - blkbits);
		if (page_no) {	/* exclude the header page */
			if (first_block < lowest_block)
				lowest_block = first_block;
			if (first_block > highest_block)
				highest_block = first_block;
		}

		/*
		 * We found a PAGE_SIZE-length, PAGE_SIZE-aligned run of blocks
		 */
		ret = add_swap_extent(sis, page_no, 1, first_block);
		if (ret < 0)
			goto out;
		nr_extents += ret;
		page_no++;
		probe_block += blocks_per_page;
reprobe:
		continue;
	}
	ret = nr_extents;
	*span = 1 + highest_block - lowest_block;
	if (page_no == 0)
		page_no = 1;	/* force Empty message */
	sis->max = page_no;
	sis->pages = page_no - 1;
	sis->highest_bit = page_no - 1;
out:
	return ret;
bad_bmap:
	pr_err("swapon: swapfile has holes\n");
	ret = -EINVAL;
	goto out;
}

static inline void count_swpout_vm_event(struct page *page);

/*
 * We may have stale swap cache pages in memory: notice
 * them here and get rid of the unnecessary final write.
 */
int swap_writepage(struct page *page, struct writeback_control *wbc)
{
	int ret = 0;

	if (try_to_free_swap(page)) {
		unlock_page(page);
		goto out;
	}
	if (frontswap_store(page) == 0) {
		set_page_writeback(page);
		unlock_page(page);
		end_page_writeback(page);
		goto out;
	}
#ifdef CONFIG_ZRAM_KCOMPRESSD
	if (sysctl_kcompressd && current_is_kswapd()) {
		struct swap_info_struct *sis = page_swap_info(page);
		pg_data_t *pgdat = page_pgdat(page);
		struct bio *bio;

		if (READ_ONCE(pgdat->kcompressd) && sis->bdev &&
		    !strncmp(sis->bdev->bd_disk->disk_name, "zram", 4)) {
			bio = get_swap_bio(GFP_NOIO, page, end_swap_bio_write);
			if (bio) {
				bio->bi_opf = REQ_OP_WRITE | REQ_SWAP |
					wbc_to_write_flags(wbc);
				bio_associate_blkg_from_page(bio, page);
				count_swpout_vm_event(page);

				/* Keep the page alive while the bio waits in our queue. */
				get_page(page);
				set_page_writeback(page);
				unlock_page(page);

				if (kcompress_enqueue(pgdat, bio)) {
					wake_up(&pgdat->kcompressd_wait);
				} else {
					submit_bio(bio);
					put_page(page);
				}
				goto out;
			}
		}
	}
#endif
	ret = __swap_writepage(page, wbc, end_swap_bio_write);
out:
	return ret;
}

#ifdef CONFIG_ZRAM_KCOMPRESSD
int kcompressd(void *p)
{
	pg_data_t *pgdat = p;
	struct bio *bio;

	/* PF_KSWAPD would make writes from this thread recurse into the queue. */
	current->flags |= PF_MEMALLOC | PF_SWAPWRITE;
	set_freezable();

	while (!kthread_should_stop()) {
		wait_event_freezable(pgdat->kcompressd_wait,
				kthread_should_stop() ||
				!kcompress_queue_empty(pgdat));

		while ((bio = kcompress_dequeue(pgdat)) != NULL) {
			struct page *page = bio_first_page_all(bio);

			submit_bio(bio);
			put_page(page);
			cond_resched();
		}
	}

	/* All producers are stopped before kthread_stop(), so this is finite. */
	while ((bio = kcompress_dequeue(pgdat)) != NULL) {
		struct page *page = bio_first_page_all(bio);

		submit_bio(bio);
		put_page(page);
		cond_resched();
	}

	return 0;
}
#endif

static inline void count_swpout_vm_event(struct page *page)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (unlikely(PageTransHuge(page)))
		count_vm_event(THP_SWPOUT);
#endif
	count_vm_events(PSWPOUT, hpage_nr_pages(page));
}

int __swap_writepage(struct page *page, struct writeback_control *wbc,
		bio_end_io_t end_write_func)
{
	struct bio *bio;
	int ret;
	struct swap_info_struct *sis = page_swap_info(page);

	VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	if (sis->flags & SWP_FS) {
		struct kiocb kiocb;
		struct file *swap_file = sis->swap_file;
		struct address_space *mapping = swap_file->f_mapping;
		struct bio_vec bv = {
			.bv_page = page,
			.bv_len  = PAGE_SIZE,
			.bv_offset = 0
		};
		struct iov_iter from;

		iov_iter_bvec(&from, WRITE, &bv, 1, PAGE_SIZE);
		init_sync_kiocb(&kiocb, swap_file);
		kiocb.ki_pos = page_file_offset(page);

		set_page_writeback(page);
		unlock_page(page);
		ret = mapping->a_ops->direct_IO(&kiocb, &from);
		if (ret == PAGE_SIZE) {
			count_vm_event(PSWPOUT);
			ret = 0;
		} else {
			/*
			 * In the case of swap-over-nfs, this can be a
			 * temporary failure if the system has limited
			 * memory for allocating transmit buffers.
			 * Mark the page dirty and avoid
			 * rotate_reclaimable_page but rate-limit the
			 * messages but do not flag PageError like
			 * the normal direct-to-bio case as it could
			 * be temporary.
			 */
			set_page_dirty(page);
			ClearPageReclaim(page);
			pr_err_ratelimited("Write error on dio swapfile (%llu)\n",
					   page_file_offset(page));
		}
		end_page_writeback(page);
		return ret;
	}

	ret = bdev_write_page(sis->bdev, swap_page_sector(page), page, wbc);
	if (!ret) {
		count_swpout_vm_event(page);
		return 0;
	}

	ret = 0;
	bio = get_swap_bio(GFP_NOIO, page, end_write_func);
	if (bio == NULL) {
		set_page_dirty(page);
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	bio->bi_opf = REQ_OP_WRITE | REQ_SWAP | wbc_to_write_flags(wbc);
	bio_associate_blkg_from_page(bio, page);
	count_swpout_vm_event(page);
	set_page_writeback(page);
	unlock_page(page);
	submit_bio(bio);
out:
	return ret;
}

int swap_readpage(struct page *page, bool synchronous)
{
	struct bio *bio;
	int ret = 0;
	struct swap_info_struct *sis = page_swap_info(page);
	blk_qc_t qc;
	struct gendisk *disk;
	unsigned long pflags;

	VM_BUG_ON_PAGE(!PageSwapCache(page) && !synchronous, page);
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(PageUptodate(page), page);

	/*
	 * Count submission time as memory stall. When the device is congested,
	 * or the submitting cgroup IO-throttled, submission can be a
	 * significant part of overall IO time.
	 */
	psi_memstall_enter(&pflags);

	if (frontswap_load(page) == 0) {
		SetPageUptodate(page);
		unlock_page(page);
		goto out;
	}

	if (sis->flags & SWP_FS) {
		struct file *swap_file = sis->swap_file;
		struct address_space *mapping = swap_file->f_mapping;

		ret = mapping->a_ops->readpage(swap_file, page);
		if (!ret)
			count_vm_event(PSWPIN);
		goto out;
	}

	ret = bdev_read_page(sis->bdev, swap_page_sector(page), page);
	if (!ret) {
		count_vm_event(PSWPIN);
		goto out;
	}

	ret = 0;
	bio = get_swap_bio(GFP_KERNEL, page, end_swap_bio_read);
	if (bio == NULL) {
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	disk = bio->bi_disk;
	/*
	 * Keep this task valid during swap readpage because the oom killer may
	 * attempt to access it in the page fault retry time check.
	 */
	bio_set_op_attrs(bio, REQ_OP_READ, 0);
	if (synchronous) {
		bio->bi_opf |= REQ_HIPRI;
		get_task_struct(current);
		bio->bi_private = current;
	}
	count_vm_event(PSWPIN);
	bio_get(bio);
	qc = submit_bio(bio);
	while (synchronous) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (!READ_ONCE(bio->bi_private))
			break;

		if (!blk_poll(disk->queue, qc, true))
			io_schedule();
	}
	__set_current_state(TASK_RUNNING);
	bio_put(bio);

out:
	psi_memstall_leave(&pflags);
	return ret;
}

int swap_set_page_dirty(struct page *page)
{
	struct swap_info_struct *sis = page_swap_info(page);

	if (sis->flags & SWP_FS) {
		struct address_space *mapping = sis->swap_file->f_mapping;

		VM_BUG_ON_PAGE(!PageSwapCache(page), page);
		return mapping->a_ops->set_page_dirty(page);
	} else {
		return __set_page_dirty_no_writeback(page);
	}
}
