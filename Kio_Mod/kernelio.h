/*
 *  Copyright (c) 2014 HCL Technologies, inc.  All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 1.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program;
 */

#ifndef __DWORKING_H__
#define __DWORKING_H__

#define KIO_BASE_NAME		"kio"

#define MAX_NAME_SIZE		30
#define MAX_PATTERN_SIZE	8
#define MAX_DEV_CNT		8
#define MAX_BIO_VEC_CNT		64
#define KIO_PG_SIZE		4096

#define SEQ_READ		1
#define SEQ_WRITE		2
#define RAND_READ		3
#define RAND_WRITE		4

#define gb_size_bytes		9223372036854775807		
//#define gb_size_bytes		1024

struct total_bw {
	unsigned long bw;
	unsigned long abw;
	int unit;
};

struct total_io_cnt {
	unsigned long io_cnt;
	unsigned long aio_cnt;
	int unit;
};

struct perf_metrics {
	unsigned long min_bw;
	unsigned long max_bw;
	unsigned long min_iops;
	unsigned long max_iops;
	unsigned long min_lat;
	unsigned long max_lat;
	struct total_bw tbw;
	struct total_io_cnt tio_cnt;
};

struct user_data {
	char devicename[MAX_NAME_SIZE];
        unsigned long runtime;
        char pattern[MAX_PATTERN_SIZE];
        bool pattern_based;
        unsigned short rw;
        unsigned int bs;
	unsigned int check_crc;
	struct perf_metrics perf;
};

struct bio_lat_list {
	//unsigned long bio_start_time;
	bool bio_complete;
	struct timeval bio_start_time;
	struct timeval bio_end_time;
	//unsigned long bio_end_time;
	struct io_load *ioload;
	struct list_head bhead;
};

struct io_load_group {
	int devcnt;
	int totaljobcnt;
	spinlock_t kio_pending;
	int io_pending;
	struct completion io_completion;
	int load_list[8];
};

struct io_load {
	int used_pool;
	unsigned int freepg[8];
	unsigned short order;
	short vec_cnt;
	int running;
	long long devsize;
	struct block_device *bdev;
	struct user_data udata;
	struct work_struct io_work;
	struct io_load_group *grp;
	unsigned long cur_bw;
	unsigned long cur_io_cnt;
	bool initial;
};

struct driver_data {
        int devcount;
        struct user_data *udata;
};

struct hrtimer iotimer;

static void kio_seq_read(struct work_struct *work);
static void kio_seq_write(struct work_struct *work);
static void kio_rand_read(struct work_struct *work);
static void kio_rand_write(struct work_struct *work);

static bool checkAvailable(void);
static struct io_load_group *getProcessGroup(void);
static int fillTargetDevice(struct io_load *iload);
static int startIOLoad(struct io_load *ioload);
static int fillIOLoad(struct io_load_group *iogrp, struct driver_data __user *argp);
static void check_bio_end(struct bio *bio, int err);
static int handleIOLoad(struct driver_data __user *argp);
static long kio_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);

#define KIO_IOCTL_NUM		127
#define KIO_MAGIC_IOC		'N'

#define IO_LOAD _IOWR(KIO_IOCTL_NUM, KIO_MAGIC_IOC, struct driver_data *)

#endif /* __DWORKING_H__ */
