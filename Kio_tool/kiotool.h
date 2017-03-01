#define SEQ_READ 			1
#define SEQ_WRITE 			2
#define RAND_READ 			3
#define RAND_WRITE 			4
#define VERIFY_CRC			5

#define DEF_RUNTIME			300000		/* Default 5 minutes/300000 msecs , when unspecified by user */
#define DEF_BS				4096		/* Default block size is 4096 or 4k */

#define TRUE				1
#define FALSE				0

#define SUCCESS				0
#define ERR				-1

#define NO_MATCH			101

#define MAX_PATTERN_SIZE		8		/* Max size of user specified pattern */
#define MAX_VALID_BS_CNT		2		/* Max number of blocksize values. we spport 4k and 8k bs only */
#define MAX_DEVICE_CNT			8		/* Max number of devices for which IO can be run */
#define MAX_TIME_MSECS			4294967295	/* Max number of milliseconds for which IO can be run. */
#define MAX_NAME_SIZE			30		/* Max size of a devicename. */
#define PAGE_SIZE			4096

#define STR_NO_WORK_LOAD_DEFINED	"No workload defined.\n"
#define STR_INVALID_ARGUMENTS		"Invalid Arguments.\n"

struct user_data {
	char devicename[MAX_NAME_SIZE];
	unsigned long runtime;
	char pattern[MAX_PATTERN_SIZE];
	bool pattern_based;
	unsigned short rw;
	unsigned int bs;
	unsigned int check_crc;
};

struct driver_data {
	int devcount;
	struct user_data *udata;
};

struct rw_value_pair {
	char *rw;
	unsigned short value;
};

struct bs_order_pair {
	int bs;
	short order;
};

struct valid_options {
	char *option;
	unsigned int optval;
};

int checkValue(int opt, char *val);

#define KIO_IOCTL_NUM           127
#define KIO_MAGIC_IOC           'N'

#define IO_LOAD _IOWR(KIO_IOCTL_NUM, KIO_MAGIC_IOC, struct driver_data *)
