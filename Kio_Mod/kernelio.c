/*
   kio.c - Generates IO load on block devices in the form of bio  

   Copyright 2014 HCL Technologies, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program;
*/

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/string.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/workqueue.h>
#include <linux/mempool.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/errno.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/bio.h>
#include <linux/preempt.h>
#include <linux/list.h>
#include <linux/genhd.h>

#include "kernelio.h"

#define KIO_DRIVER_VERSION "1.0"

static DEFINE_SPINLOCK(kio_main_lock);
static DEFINE_SPINLOCK(bio_lat_list_lock);

static LIST_HEAD(gbio_lat_list);

static long kio_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);

MODULE_AUTHOR("Rajiv Shanmugam Madeswaran<rajiv.sm@hcl.com>, Sahana Jagadeesan <sahana.ja@hcl.com>");
MODULE_DESCRIPTION("Kernel level IO tool");
MODULE_LICENSE("GPL");
MODULE_VERSION(KIO_DRIVER_VERSION);

struct io_load_group iogrp[MAX_DEV_CNT];
struct io_load load[MAX_DEV_CNT];

struct workqueue_struct *kio_queue;
mempool_t *kio_page_pool;
int devcnt = 0;
int done = 0;

static const struct file_operations kio_fops =
{
        .owner          = THIS_MODULE,
        .unlocked_ioctl = kio_ioctl,
};

static struct miscdevice kio_miscdev =
{
        MISC_DYNAMIC_MINOR,
        "kio",
        &kio_fops
};

unsigned long
randomize_range(unsigned long start, unsigned long end, unsigned long len)
{
	unsigned long range = end - len - start;

	if (end <= start + len)
		return 0;

	return PAGE_ALIGN(get_random_int() % range + start);
}

static void update_total_io(struct perf_metrics *perf, unsigned int bio_bw, struct io_load *iload)
{
	struct total_bw *tbw;
	struct total_io_cnt *tiocnt;
	struct io_load *curload = iload;

	tbw = &perf->tbw;
	tiocnt = &perf->tio_cnt;

	if ((tbw->bw + bio_bw) >= gb_size_bytes) {
		tbw->unit = tbw->unit + ((tbw->bw + bio_bw) / gb_size_bytes);
		tbw->bw = tbw->bw + ((tbw->bw + bio_bw) % gb_size_bytes);
	} else
		tbw->bw = tbw->bw + bio_bw;

	if ((tiocnt->io_cnt + curload->vec_cnt) >= gb_size_bytes) {
		tiocnt->unit = tiocnt->unit + ((tiocnt->io_cnt + curload->vec_cnt) / gb_size_bytes);
		tiocnt->io_cnt = tiocnt->io_cnt + ((tiocnt->io_cnt + curload->vec_cnt) % gb_size_bytes);
        } else
		tiocnt->io_cnt = tiocnt->io_cnt + curload->vec_cnt;
}

static void update_bw_iops(struct perf_metrics *perf, struct io_load *curload)
{
	if (unlikely(curload->initial == 1)) {
		perf->min_bw = curload->cur_bw;
		perf->max_bw = curload->cur_bw;
		perf->min_iops = curload->cur_io_cnt;
		perf->max_iops = curload->cur_io_cnt;
	}
	else {
	if (curload->cur_bw < perf->min_bw)
		perf->min_bw = curload->cur_bw;
	
	if (curload->cur_bw > perf->max_bw)
		perf->max_bw = curload->cur_bw;

	if (curload->cur_io_cnt < perf->min_iops)
		perf->min_iops = curload->cur_io_cnt;

	if (curload->cur_io_cnt > perf->max_iops)
		perf->max_iops = curload->cur_io_cnt;
	}

	curload->cur_bw = 0;
	curload->cur_io_cnt = 0;	
	curload->initial = 0;	
}

static enum hrtimer_restart update_io_stats(struct hrtimer *timer)
{
	struct io_load *curload;
	struct user_data *ud;
	struct perf_metrics *perf;
	struct bio_lat_list *bio_entry;
	struct bio_lat_list *temp;
	unsigned long cur_lat = 0, cur_lat_sec = 0;
	int i = 0;
	unsigned long flags;

	spin_lock_irqsave(&bio_lat_list_lock, flags);
	for (i = 0; i < MAX_DEV_CNT; i++) {
		if (load[i].running) {
			ud = &load[i].udata;
			perf = &ud->perf;
			curload = &load[i];
		
			update_bw_iops(perf, curload);
		}
	}
	
	list_for_each_entry_safe(bio_entry, temp, &gbio_lat_list, bhead) {
		if (bio_entry->bio_complete) {
			curload = bio_entry->ioload;
			ud = &curload->udata;
			perf = &ud->perf;
			
			cur_lat_sec = bio_entry->bio_end_time.tv_sec - bio_entry->bio_start_time.tv_sec;
			cur_lat = (cur_lat_sec * 1000000) + (bio_entry->bio_end_time.tv_usec - bio_entry->bio_start_time.tv_usec);

			if (cur_lat > 0) {
				cur_lat = cur_lat / curload->vec_cnt;
			
				if (unlikely(curload->initial == 1)) {
		                	perf->min_lat = cur_lat;
                			perf->max_lat = cur_lat;
				} else {
					if (cur_lat > perf->max_lat)
						perf->max_lat = cur_lat;

					if (cur_lat < perf->min_lat)
						perf->min_lat = cur_lat;
				} 
			}
			
			list_del(&bio_entry->bhead);
			kfree(bio_entry);
		}
	}
	cur_lat = 0;

	//if (curload && done)
	//	curload->initial = 0;

	spin_unlock_irqrestore(&bio_lat_list_lock, flags);

	hrtimer_forward_now(timer, ktime_set(1, 0));
	return HRTIMER_RESTART;
}

static bool checkAvailable(void)
{
	int i = 0, start = 0;
	bool ret = 0;
	int found = 0;

	for (i = 0; i < devcnt; i++) {
		while (start < MAX_DEV_CNT) {
			if (!load[start].running) {
				found++;
				start++;
				break;
			} else
				start++;
		}
	}

	if (devcnt == found)
		ret = 1;

	return ret;
}

static struct io_load_group *getProcessGroup(void)
{
	int i = 0;

	for (i = 0; i < MAX_DEV_CNT; i++) {
		if (iogrp[i].devcnt == 0) {
			iogrp[i].devcnt = devcnt;
			iogrp[i].totaljobcnt = devcnt;
			iogrp[i].io_pending = 0;
			spin_lock_init(&iogrp[i].kio_pending);
			return &iogrp[i];
		}
	}

	return NULL;
}

static int fillTargetDevice(struct io_load *iload)
{
	struct file *filp = NULL;
	struct inode *inode = NULL;
	struct user_data ud;
	int ret = -EFAULT;

	ud = iload->udata;
	
	filp = filp_open(ud.devicename, O_RDONLY | O_LARGEFILE, 0);
        if (IS_ERR(filp))
        	goto file_out;

        inode = file_inode(filp);
        if (!S_ISBLK(inode->i_mode)) {
        	goto file_close;
        }

        iload->bdev = inode->i_bdev;
        iload->devsize = i_size_read(inode->i_mapping->host);

        if (iload->devsize < 0) {
        	goto file_close;
        }

	ret = 0;

	file_close:
		fput(filp);
	file_out:
		return ret;
}

static int startIOLoad(struct io_load *ioload)
{
	struct user_data udata;
	int ret = 0;

	udata = ioload->udata;

	switch (udata.rw) {
	case SEQ_READ:
		INIT_WORK_ONSTACK(&ioload->io_work, kio_seq_read);
	break;
	
	case SEQ_WRITE:
		INIT_WORK_ONSTACK(&ioload->io_work, kio_seq_write);
	break;
	
	case RAND_READ:
		INIT_WORK_ONSTACK(&ioload->io_work, kio_rand_read);
	break;

	case RAND_WRITE:
		INIT_WORK_ONSTACK(&ioload->io_work, kio_rand_write);
	break;

	default:
		return -EFAULT;	
	}
	
	queue_work(kio_queue, &ioload->io_work);
	
	return ret;
}
static int fillIOStats(struct io_load_group *iogrp, struct driver_data __user *argp)
{
	struct io_load *curload;
	struct user_data *ud;
	struct user_data *dest_ud;
	struct total_bw *tbw;
	struct total_io_cnt *tio_cnt;
	struct perf_metrics *pm;
	int i = 0, index = 0;

	curload = &load[0];
	dest_ud = argp->udata;

	printk(KERN_INFO "Total jobcount is %d\n",iogrp->totaljobcnt);
	for (i = 0; i < iogrp->totaljobcnt; i++) {
		index = iogrp->load_list[i];

		printk(KERN_INFO "Filling IO Stats: %d for index %d\n",i,index);
		if (index != -1) {
			ud = &curload[index].udata;
			pm = &ud->perf;
			tbw = &pm->tbw;
			tio_cnt = &pm->tio_cnt;
			printk(KERN_INFO "Total BW : %lu\n",tbw->abw);
			printk(KERN_INFO "Total IOPS : %lu\n",tio_cnt->aio_cnt);
			printk(KERN_INFO "Total BW in units: %lu with unit %d\n",tbw->bw,tbw->unit);
			printk(KERN_INFO "Total IOPS in units: %lu with unit %d\n",tio_cnt->io_cnt, tio_cnt->unit);
			printk(KERN_INFO "min and max bw : %lu and %lu\n",pm->min_bw,pm->max_bw);
			printk(KERN_INFO "min and max iops : %lu and %lu\n",pm->min_iops, pm->max_iops);
			printk(KERN_INFO "min and max latency : %lu and %lu\n",pm->min_lat,pm->max_lat);
			if (copy_to_user(dest_ud,ud, sizeof(struct user_data))) {
				printk(KERN_INFO "kio : error while copying back to user\n");
				return -1;
			} else {
				printk(KERN_INFO "Succeeded copying %d for index %d\n",i,index);
			}
		}
		dest_ud++;
		iogrp->load_list[i] = -1;
	}	
	
	return 0;	
}

static int fillIOLoad(struct io_load_group *iogrp, struct driver_data __user *argp)
{
	struct io_load *curload;
	struct user_data *u;
	int i = 0, start = 0;
        bool ret = 0;

        for (i = 0; i < iogrp->devcnt; i++) {
        	while (start < MAX_DEV_CNT) {
                	if (!load[start].running) {
                		ret = copy_from_user(&load[start].udata, &argp->udata[i], sizeof(struct user_data));
                		if (ret) {
                        		printk(KERN_INFO "kio error while copying\n");
                        		ret = -EFAULT;
					goto munlock;
                		}

                		u = &load[start].udata;
				curload = &load[start];
               			curload->used_pool = 0;
				memset(curload->freepg, 0, (sizeof(unsigned int) * 8));
				curload->running = 1;

				curload->cur_bw = 0;
				curload->cur_io_cnt = 0;
				curload->initial = 1;

				ret = fillTargetDevice(&load[start]);
				if (ret < 0)
					goto munlock;

				curload->grp = iogrp;
				
				iogrp->load_list[i] = start;
					
				ret = startIOLoad(&load[start]);
				if (ret < 0)
					goto munlock;

				start++;
                                break;
                        } else
                                start++;
                }
	}
	done = 1;

	munlock:
		spin_unlock_irq(&kio_main_lock);

		return ret;
}

/*
 * check_bio_end is the callback function that gets called when a bio completes processing.
 */
static void check_bio_end(struct bio *bio, int err)
{
        struct bio_lat_list *lat_list = bio->bi_private;
	struct io_load *iload = lat_list->ioload;
	struct io_load_group *iogrp;
	struct bio_vec bv;
	struct bvec_iter iter;
	struct user_data *ud = &iload->udata;
	struct perf_metrics *perf = &ud->perf;
	unsigned long flags;
	
	iogrp = iload->grp;
	do_gettimeofday(&lat_list->bio_end_time);

	spin_lock_irqsave(&bio_lat_list_lock, flags);
	lat_list->bio_complete = 1;
	
	iload->cur_bw = iload->cur_bw + bio->bi_iter.bi_size;
	iload->cur_io_cnt = iload->cur_io_cnt + iload->vec_cnt;

	update_total_io(perf, bio->bi_iter.bi_size, iload);
	spin_unlock_irqrestore(&bio_lat_list_lock, flags);	
	
	spin_lock_irqsave(&iogrp->kio_pending, flags);
	iogrp->io_pending = iogrp->io_pending - 1;
	spin_unlock_irqrestore(&iogrp->kio_pending, flags);

	printk(KERN_INFO "before check biofree\n");
	preempt_disable();	
	bio_for_each_segment(bv, bio, iter) {
		printk(KERN_INFO "going to free page\n");
		__free_pages(bv.bv_page, iload->order);
	}
        bio_put(bio);
	
	preempt_enable();
	printk(KERN_INFO "after check biofree\n");
}

/*
 * seq_read is the work handler function that generates bio's performing sequential reads.
 */ 
static void kio_seq_read(struct work_struct *work)
{
        struct io_load *ioload;
        struct user_data ud;
        struct io_load_group *iogrp;
        unsigned long timeout;
	unsigned long dev_size = 0;
        unsigned long last_bio_sector = 0;
        unsigned long bio_start_sector = 0;
        unsigned int pgcopy_size = 0;
        int vec_cnt = MAX_BIO_VEC_CNT;
        short order = 0;
        int i = 0;
        struct bio *bio;
        struct page *page;
	struct bio_lat_list *bio_lat;
	unsigned long flags;
	bool done = 0;
	int k = 0;

        ioload = container_of(work, struct io_load, io_work);
        iogrp = ioload->grp;
        ud = ioload->udata;

	dev_size = ioload->devsize;
        last_bio_sector = (dev_size - ((MAX_BIO_VEC_CNT * KIO_PG_SIZE))) >> 9;
        order = ud.bs;
        pgcopy_size = KIO_PG_SIZE << order;
	ioload->order = order;
	ioload->vec_cnt = MAX_BIO_VEC_CNT >> order;

        timeout = jiffies + msecs_to_jiffies(ud.runtime);

	printk(KERN_INFO "before timeout in seq read\n");
        do {
		if (k <= 0) {
		bio = bio_alloc(GFP_ATOMIC | __GFP_WAIT, vec_cnt);
                if (!bio) {
                        printk(KERN_ERR "kio : seq read : No memory for allocating bio\n");
                        goto out;
                }

		printk(KERN_INFO "allocated bio..\n");
                bio->bi_iter.bi_sector = bio_start_sector;
                bio->bi_bdev = ioload->bdev;
                bio->bi_end_io = check_bio_end;

		spin_lock_irqsave(&bio_lat_list_lock, flags);	
		bio_lat = kzalloc(sizeof(*bio_lat), GFP_ATOMIC);
		do_gettimeofday(&bio_lat->bio_start_time);
		bio_lat->bio_complete = 0;
		bio_lat->ioload = ioload;
		INIT_LIST_HEAD(&bio_lat->bhead);
		
		list_add_tail(&bio_lat->bhead, &gbio_lat_list);	
                bio->bi_private = list_last_entry(&gbio_lat_list, struct bio_lat_list, bhead);
		spin_unlock_irqrestore(&bio_lat_list_lock, flags);

                for (i = 0; i < ioload->vec_cnt; i++) {
			printk(KERN_INFO "allocating page %d\n",i);	
                        page = alloc_pages(GFP_ATOMIC | __GFP_ZERO, order);
                        if (!page) {
                                printk(KERN_ERR "kio :  seq read : No memory for allocating the page\n");
                                goto deref;
                        }
                        
                        if (!bio_add_page(bio, page, pgcopy_size, 0)) {
                                if (bio->bi_vcnt == 0) {
                                        printk(KERN_ERR "kio : seq read : could not add the page to bio\n");
                                        goto deref;
                                }
                        }
                }

		spin_lock_irqsave(&iogrp->kio_pending, flags);
		iogrp->io_pending = iogrp->io_pending + 1;
		spin_unlock_irqrestore(&iogrp->kio_pending, flags);
                
		submit_bio(READ, bio);

                bio_start_sector = bio_start_sector + ((pgcopy_size * vec_cnt) >> 9);
		if (bio_start_sector > last_bio_sector) {
			printk(KERN_INFO "how often here\n");
                        bio_start_sector = 0;
		}
		//udelay(200);
		//msleep(3);
		k++;
		}
		msleep(100);
        } while (time_before(jiffies, timeout));
        printk(KERN_INFO " after timeout in seq read going to release lock\n");
	goto out;
	printk(KERN_INFO "going to defref\n");
	
	deref:
		bio_put(bio);
	out:
		printk(KERN_INFO "in out part\n");
        	spin_lock_irqsave(&kio_main_lock, flags);
        	ioload->running = 0;
		spin_unlock_irqrestore(&kio_main_lock, flags);

		printk(KERN_INFO "seq read devcnt is %d\n",iogrp->devcnt);
		spin_lock_irqsave(&iogrp->kio_pending, flags);
        	iogrp->devcnt = iogrp->devcnt - 1;
		printk(KERN_INFO "here seq read devcnt is %d\n",iogrp->devcnt);

        	if ((iogrp->devcnt == 0)) {
			spin_unlock_irqrestore(&iogrp->kio_pending, flags);
			
			while (done != 1) {
				if (spin_trylock(&iogrp->kio_pending)) {
					if (iogrp->io_pending == 0) {
						done = 1;
						printk(KERN_INFO "wrok handler seq read is going to call ocmplete\n");
						complete(&iogrp->io_completion);
					} 
					spin_unlock(&iogrp->kio_pending);
				}
			}
		} else 
			spin_unlock_irqrestore(&iogrp->kio_pending, flags);
}

/*
 * seq_write is the work handler function that generates bio's performing sequential writes.
 */
static void kio_seq_write(struct work_struct *work)
{
        struct io_load *ioload;
        struct user_data ud;
        struct io_load_group *iogrp;
        unsigned long timeout;
	unsigned long dev_size = 0;
	unsigned long last_bio_sector = 0;
	unsigned long bio_start_sector = 0;
	unsigned int pgcopy_size = 0;
	int vec_cnt = MAX_BIO_VEC_CNT;
	short order = 0;
	int copylen = 0, str_len = 0, i = 0;
	char *pageaddr = NULL;
	char *incaddr = NULL;
	struct bio *bio;
	struct page *page;
	struct bio_lat_list *bio_lat;
	unsigned long flags;
	bool done = 0;

        ioload = container_of(work, struct io_load, io_work);
        iogrp = ioload->grp;
        ud = ioload->udata;

	dev_size = ioload->devsize;
	last_bio_sector = (dev_size - ((MAX_BIO_VEC_CNT) * (KIO_PG_SIZE))) >> 9;
	order = ud.bs;
	pgcopy_size = KIO_PG_SIZE << order;
	ioload->vec_cnt = MAX_BIO_VEC_CNT >> order;
	ioload->order = order;

        timeout = jiffies + msecs_to_jiffies(ud.runtime);
	printk(KERN_INFO "before timeout in seq write\n");
        do {
		bio = bio_alloc(GFP_ATOMIC | __GFP_WAIT, vec_cnt);
		if (!bio) {
			//errno = -ENOMEM;
			printk(KERN_ERR "kio : : seq write : No memory for allocating bio\n");
			goto out;
		}
		
		bio->bi_iter.bi_sector = bio_start_sector;	
		bio->bi_bdev = ioload->bdev;
		bio->bi_end_io = check_bio_end;
		
		spin_lock_irqsave(&bio_lat_list_lock, flags);
                bio_lat = kzalloc(sizeof(*bio_lat), GFP_ATOMIC);
                do_gettimeofday(&bio_lat->bio_start_time);
                bio_lat->bio_complete = 0;
                bio_lat->ioload = ioload;
                INIT_LIST_HEAD(&bio_lat->bhead);

                list_add_tail(&bio_lat->bhead, &gbio_lat_list);
                bio->bi_private = list_last_entry(&gbio_lat_list, struct bio_lat_list, bhead);
                spin_unlock_irqrestore(&bio_lat_list_lock, flags);

		for (i = 0; i < ioload->vec_cnt; i++) {
			page = alloc_pages(GFP_ATOMIC | __GFP_ZERO, order);
			if (!page) {
				printk(KERN_ERR "kio : seq write : No memory for allocating the page\n");
				goto out;
			}
			
			pageaddr = page_address(page);
			incaddr = pageaddr;
			if (ud.pattern_based) {
				str_len = strlen(ud.pattern);
				copylen = str_len;
				while (copylen < pgcopy_size) {
					memcpy(incaddr, ud.pattern, str_len);
					incaddr = incaddr + str_len;
					copylen += str_len;
				}
			} else
				get_random_bytes((char *) pageaddr, pgcopy_size); 
		
			if (!bio_add_page(bio, page, pgcopy_size, 0)) {
				if (bio->bi_vcnt == 0) {
					printk(KERN_ERR "kio : : seq write : could not add the page to bio\n");
					goto out;
				}
			}
		}
		spin_lock_irqsave(&iogrp->kio_pending, flags);
		iogrp->io_pending = iogrp->io_pending + 1;
		spin_unlock_irqrestore(&iogrp->kio_pending, flags);

		submit_bio(WRITE, bio);	
		bio_start_sector = bio_start_sector + ((pgcopy_size * vec_cnt) >> 9); 	
		if (bio_start_sector > last_bio_sector) {
			printk(KERN_INFO "start over again\n");
			bio_start_sector = 0;
		}
       		//udelay(200);
       		//msleep(3);
	} while (time_before(jiffies, timeout));
	printk(KERN_INFO "timeout has occured in seq write\n");
	out:
		printk(KERN_INFO "in out part\n");
        	spin_lock_irqsave(&kio_main_lock, flags);
       		ioload->running = 0;
		spin_unlock_irqrestore(&kio_main_lock, flags);

		printk(KERN_INFO "seq write devcnt is %d\n",iogrp->devcnt);
		spin_lock_irqsave(&iogrp->kio_pending, flags);
        	iogrp->devcnt = iogrp->devcnt - 1;
		printk(KERN_INFO "here seq write devcnt is %d\n",iogrp->devcnt);
        	if ((iogrp->devcnt == 0)) {
			spin_unlock_irqrestore(&iogrp->kio_pending, flags);

			while (done != 1) {
				if (spin_trylock(&iogrp->kio_pending)) {
					if (iogrp->io_pending == 0) {
						done = 1;
						printk(KERN_INFO "wrok handler seq write is going to call ocmplete\n");
						complete(&iogrp->io_completion);
					}
					spin_unlock(&iogrp->kio_pending);
				}
			}
		} else 
			spin_unlock_irqrestore(&iogrp->kio_pending, flags);
}

/*
 * rand_read is the work handler function that generates bio's performing random reads.
 */
static void kio_rand_read(struct work_struct *work)
{
        struct io_load *ioload;
        struct user_data ud;
        struct io_load_group *iogrp;
        unsigned long timeout;
	unsigned long dev_size = 0;
        unsigned long last_bio_sector = 0;
        unsigned long bio_start_sector = 0;
        unsigned int pgcopy_size = 0;
        short order = 0;
        struct bio *bio;
        struct page *page;
	struct bio_lat_list *bio_lat;
	unsigned long flags;
	bool done = 0;

        ioload = container_of(work, struct io_load, io_work);
        iogrp = ioload->grp;
        ud = ioload->udata;

	dev_size = ioload->devsize;
        order = ud.bs;
	last_bio_sector = (dev_size - (KIO_PG_SIZE << order)) >> 9;
        pgcopy_size = KIO_PG_SIZE << order;
	ioload->order = order;
	ioload->vec_cnt = 1;

        timeout = jiffies + msecs_to_jiffies(ud.runtime);
	printk(KERN_INFO "before timeout in rand read\n");
        do {
		bio = bio_alloc(GFP_ATOMIC | __GFP_WAIT, 1);
                if (!bio) {
                        //errno = -ENOMEM;
                        printk(KERN_ERR "kio : No memory for allocating bio\n");
                        goto out;
                }

                bio->bi_iter.bi_sector = bio_start_sector;
                bio->bi_bdev = ioload->bdev;
                bio->bi_end_io = check_bio_end;
		
		spin_lock_irqsave(&bio_lat_list_lock, flags);
                bio_lat = kzalloc(sizeof(*bio_lat), GFP_ATOMIC);
                do_gettimeofday(&bio_lat->bio_start_time);
	        bio_lat->bio_complete = 0;
                bio_lat->ioload = ioload;
                INIT_LIST_HEAD(&bio_lat->bhead);

                list_add_tail(&bio_lat->bhead, &gbio_lat_list);
                bio->bi_private = list_last_entry(&gbio_lat_list, struct bio_lat_list, bhead);
                spin_unlock_irqrestore(&bio_lat_list_lock, flags);

                page = alloc_pages(GFP_ATOMIC | __GFP_ZERO, order);
                if (!page) {
                	printk(KERN_ERR "kio : : rand read : No memory for allocating the page\n");
                	goto out;
                }

                if (!bio_add_page(bio, page, pgcopy_size, 0)) {
                	if (bio->bi_vcnt == 0) {
                        	printk(KERN_ERR "kio : rand read : could not add the page to bio\n");
                               	goto out;
                       	}
                }
		spin_lock_irqsave(&iogrp->kio_pending, flags);
		iogrp->io_pending = iogrp->io_pending + 1;
		spin_unlock_irqrestore(&iogrp->kio_pending, flags);

                submit_bio(READ, bio);
                bio_start_sector = (randomize_range(0, dev_size, 0)) >> 9;
		//udelay(200);
		//msleep(3);
        } while (time_before(jiffies, timeout));

	printk(KERN_INFO "timeout has occured in randread\n");
	out:
		printk(KERN_INFO "in out part of rand read \n");
        	spin_lock_irqsave(&kio_main_lock, flags);
        	ioload->running = 0;
		spin_unlock_irqrestore(&kio_main_lock, flags);

		printk(KERN_INFO "rand read devcnt is %d\n",iogrp->devcnt);
		spin_lock_irqsave(&iogrp->kio_pending, flags);
        	iogrp->devcnt = iogrp->devcnt - 1;
		printk(KERN_INFO "here rand read devcnt is %d\n",iogrp->devcnt);
		if ((iogrp->devcnt == 0)) {
                        spin_unlock_irqrestore(&iogrp->kio_pending, flags);

                        while (done != 1) {
                                if (spin_trylock(&iogrp->kio_pending)) {
                                        if (iogrp->io_pending == 0) {
                                                done = 1;
                                                printk(KERN_INFO "wrok handler rand read is going to call ocmplete\n");
                                                complete(&iogrp->io_completion);
                                        }
                                        spin_unlock(&iogrp->kio_pending);
                                }
                        }
                } else
                        spin_unlock_irqrestore(&iogrp->kio_pending, flags);
}

/*
 * rand_write is the work handler function that generates bio's performing random writes.
 */
static void kio_rand_write(struct work_struct *work)
{
        struct io_load *ioload;
        struct user_data ud;
        struct io_load_group *iogrp;
        unsigned long timeout;
	unsigned long dev_size = 0;
        unsigned long last_bio_sector = 0;
        unsigned long bio_start_sector = 0;
        unsigned int pgcopy_size = 0;
        short order = 0;
        int copylen = 0, str_len = 0;
        char *pageaddr = NULL;
	char *incaddr = NULL;
        struct bio *bio;
        struct page *page;
	struct bio_lat_list *bio_lat;
	unsigned long flags;
	bool done = 0;

        ioload = container_of(work, struct io_load, io_work);
        iogrp = ioload->grp;
        ud = ioload->udata;

	dev_size = ioload->devsize;
        order = ud.bs;
	last_bio_sector = (dev_size - (KIO_PG_SIZE << order)) >> 9;
        pgcopy_size = KIO_PG_SIZE << order;
	ioload->order = order;
	ioload->vec_cnt = 1;

        timeout = jiffies + msecs_to_jiffies(ud.runtime);
	printk(KERN_INFO "before timeout in rand write\n");
        do {
		bio = bio_alloc(GFP_ATOMIC | __GFP_WAIT, 1);
                if (!bio) {
                        printk(KERN_ERR "kio : rand write : No memory for allocating bio\n");
                        goto out;
                }

                bio->bi_iter.bi_sector = bio_start_sector;
                bio->bi_bdev = ioload->bdev;
                bio->bi_end_io = check_bio_end;

		spin_lock_irqsave(&bio_lat_list_lock, flags);
                bio_lat = kzalloc(sizeof(*bio_lat), GFP_ATOMIC);
                do_gettimeofday(&bio_lat->bio_start_time);
                bio_lat->bio_complete = 0;
                bio_lat->ioload = ioload;
                INIT_LIST_HEAD(&bio_lat->bhead);

                list_add_tail(&bio_lat->bhead, &gbio_lat_list);
                bio->bi_private = list_last_entry(&gbio_lat_list, struct bio_lat_list, bhead);
                spin_unlock_irqrestore(&bio_lat_list_lock, flags);

                page = alloc_pages(GFP_ATOMIC | __GFP_ZERO, order);
                if (!page) {
                	printk(KERN_ERR "kio : rand write : No memory for allocating the page\n");
                        goto out;
                }

                pageaddr = page_address(page);
		incaddr = pageaddr;

                if (ud.pattern_based) {
                	str_len = strlen(ud.pattern);
                        copylen = str_len;

                        while (copylen < pgcopy_size) {
                        	memcpy(incaddr, ud.pattern, str_len);
                                incaddr = incaddr + str_len;
                                copylen += str_len;
                        }
                } else
                        get_random_bytes((char *) pageaddr, pgcopy_size);

                if (!bio_add_page(bio, page, pgcopy_size, 0)) {
                	if (bio->bi_vcnt == 0) {
                        	printk(KERN_ERR "kio : rand write : could not add the page to bio\n");
                                goto out;
                         }
                }
		spin_lock_irqsave(&iogrp->kio_pending, flags);
		iogrp->io_pending = iogrp->io_pending + 1;
		spin_unlock_irqrestore(&iogrp->kio_pending, flags);
                
		submit_bio(WRITE, bio);
                bio_start_sector = (randomize_range(0, dev_size, 0)) >> 9;
		//udelay(200);
		//msleep(3);
        } while (time_before(jiffies, timeout));
	printk(KERN_INFO "timeout has happend in rand write\n");
	out :
		printk(KERN_INFO "in out part of rand write \n");
        	spin_lock_irqsave(&kio_main_lock, flags);
        	ioload->running = 0;
		spin_unlock_irqrestore(&kio_main_lock, flags);

		printk(KERN_INFO "randwrite devcnt is %d\n",iogrp->devcnt);
		spin_lock_irqsave(&iogrp->kio_pending, flags);	
        	iogrp->devcnt = iogrp->devcnt - 1;
       		printk(KERN_INFO "here randwrite devcnt is %d\n",iogrp->devcnt); 
		if ((iogrp->devcnt == 0)) {
                        spin_unlock_irqrestore(&iogrp->kio_pending, flags);

                        while (done != 1) {
                                if (spin_trylock(&iogrp->kio_pending)) {
                                        if (iogrp->io_pending == 0) {
                                                done = 1;
                                                printk(KERN_INFO "wrok handler rand write is going to call ocmplete\n");
                                                complete(&iogrp->io_completion);
                                        }
                                        spin_unlock(&iogrp->kio_pending);
                                }
                        }
                } else
                        spin_unlock_irqrestore(&iogrp->kio_pending, flags);
}

static int handleIOLoad(struct driver_data __user *argp)
{
	struct io_load_group *iogrp;
	int ret = -EFAULT;

	//preempt_disable();
	spin_lock_irq(&kio_main_lock);
	get_user(devcnt, &argp->devcount);

	if (!checkAvailable()) {
		ret = -EAGAIN;
		goto unlock;
	}

	iogrp = getProcessGroup();
	if (iogrp == NULL)
		goto unlock;

	ret = fillIOLoad(iogrp, argp);	
	if (ret < 0)
		goto out;

	printk(KERN_INFO "we have started to wait for process to finish\n");
	init_completion(&iogrp->io_completion);
	printk(KERN_INFO "I am waiting\n");
	wait_for_completion(&iogrp->io_completion);
	printk(KERN_INFO "Waited enough time.. going to return\n");

	ret = fillIOStats(iogrp, argp);

	goto out;

	unlock:
		spin_unlock_irq(&kio_main_lock);
	out:
		return ret;
	
}

static long kio_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct driver_data __user *argp = (struct driver_data __user *)arg;
	int ret = 0;

	switch (cmd) {
	case IO_LOAD:
	        ret = handleIOLoad(argp);
		if (ret < 0)
			ret = -EFAULT;
		break;
	default:
		ret = -ENOIOCTLCMD;
	}
	
	return ret;
}

static int __init kio_init(void)
{
	int ret = 0;

	ret = misc_register(&kio_miscdev);
	if (ret < 0) {
		printk(KERN_ERR "Unable to register 'kio' misc device\n");
		return -ENODEV; 
	}
	
	memset(load, 0, (sizeof(struct io_load) * MAX_DEV_CNT));	
	memset(iogrp, 0, (sizeof(struct io_load_group) * MAX_DEV_CNT));

 	kio_queue = alloc_workqueue(KIO_BASE_NAME, WQ_UNBOUND, 0);
	//kio_queue = alloc_workqueue(KIO_BASE_NAME, WQ_HIGHPRI | WQ_CPU_INTENSIVE, 0);
	if (!kio_queue) {
		ret = -ENOMEM;
		goto unregister;
	}

	kio_page_pool = mempool_create_page_pool(MAX_BIO_VEC_CNT, 0);
	if (!kio_page_pool) {
		ret = -ENOMEM;
		goto destroy;
	}

	hrtimer_init(&iotimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	iotimer.function = update_io_stats;
	hrtimer_start(&iotimer, ktime_set(1, 0), HRTIMER_MODE_REL);

	printk(KERN_INFO "Registered kio module successfully\n");
	goto out;

	destroy:
		destroy_workqueue(kio_queue);
	unregister:
		misc_deregister(&kio_miscdev);
	out:
		return ret;
}

static void __exit kio_exit(void)
{
	misc_deregister(&kio_miscdev);
	destroy_workqueue(kio_queue);
	mempool_destroy(kio_page_pool);
	hrtimer_cancel(&iotimer);
	printk(KERN_INFO "Unregistered 'kio' misc device\n");
}

module_init(kio_init);
module_exit(kio_exit);
