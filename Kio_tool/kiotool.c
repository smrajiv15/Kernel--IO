/*
 * kiotool.c - parses workload configuration from a workload file or through command line options and sends it to target device on which IO should
 * be run.
 *
 * Copyright (c) 2014 HCL Technologies, inc.  All Rights Reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 1.1 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program;
*/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <linux/ioctl.h>

#include "kiotool.h"

struct user_data ud[MAX_DEVICE_CNT];
int optind, opt_inval = -1;
int devindex = 0;
int dev_present = 0;
int totalcnt = 1;
struct driver_data ddata;
struct user_data *work_data;
unsigned long totaltime = 0;
int done = 0;

/*
 * valid rw options.
 * */
struct rw_value_pair rw_type[] = {
	{
		.rw = (char *) "seqread",
		.value = SEQ_READ,
	},
	{
		.rw = (char *) "seqwrite",
		.value = SEQ_WRITE,
	},
	{
		.rw = (char *) "randread",
		.value = RAND_READ,
	},
	{
		.rw = (char *) "randwrite",
		.value = RAND_WRITE,
	},
	{
		.rw = (char *) "verifycrc",
		.value = VERIFY_CRC,
	},
	{
		.rw = NULL,
	}
};

/*
 * valid blocksize options.
 */
struct bs_order_pair bs_order[] = {
	{
		.bs = 4096,
		.order = 0,
	},
	{
		.bs = 8192,
		.order = 1,
	},
	{
		.bs = 16384,
		.order = 2,
	},
	{
		.bs = 32768,
		.order = 3,
	},
	{
		.bs = 65536,
		.order = 4,
	},
	{
		.bs = -1,
	}
};

/*
 * valid command line options.
 */
struct valid_options vopts[] = {
	{
		.option = (char *) "device",
		.optval = 'd',
	},
	{
		.option = (char *) "runtime",
		.optval = 't',
	},
	{
		.option = (char *) "rw",
		.optval = 'm',
	},
	{
		.option = (char *) "pattern",
		.optval = 'p',
	},
	{	
		.option = (char *) "bs",
		.optval = 'b',
	},
	{
		.option = (char *) "verify",
		.optval = 'v',
	},
	{
		.option = NULL,
	}
};

/*
 * init_user_data function initialises user_data structures.
 */
void init_user_data(void)
{
	struct user_data *data;
	int i;

	memset((struct user_data *)&ud, 0, (sizeof(struct user_data) * MAX_DEVICE_CNT));
	data = ud;
	ddata.udata = ud;
	ddata.devcount = totalcnt;
	work_data = ddata.udata;
	
	for (i = 0; i < MAX_DEVICE_CNT; i++) {
		data->runtime = DEF_RUNTIME;
		data->rw = RAND_READ;
		data->pattern_based = FALSE;
		data->bs = DEF_BS;
		data->check_crc = 0;
		data++;
	}
}	

/*
 * show_usage function shows the commandline usage of the tool.
 */
void show_usage(void)
{
	printf("Usage: kiotool [options] <workload file>\n");
	printf("\nOptions:\n");
	printf("  --device <block_dev>\t\tDevice on which IO should be generated. (Required)\n"); 
	printf("  --runtime <seconds>\t\tTime period for which the IO load should be run. Defaults to 60 seconds.\n");
	printf("  --rw <rw type>\t\tType of IO operation to be performed on the device.\n");
	printf("  \t\t\t\t\tValid rw values:\n");
	printf("  \t\t\t\t\t\tseqread   : Sequential reads\n");
	printf("  \t\t\t\t\t\tseqwrite  : Sequential writes\n");
	printf("  \t\t\t\t\t\trandread  : Random reads\n");
	printf("  \t\t\t\t\t\trandwrite : Random writes\n\t\t\t\t\tDefaults to randread.\n");
	printf("  --bs <blocksize>\t\tBlock size used for the io units. Defaults to 4k.\n");
	printf("  --pattern <data>\t\tIf set, io buffers will be filled with this pattern. Defaults to filling with random bytes.\n");
}

/*
 * get_optval function gets a value for the option from commandline.
 */
char *get_optval(int argc, char *argv[])
{
	char *carg;

	if (optind < argc) {
		carg = argv[optind];
		if (carg[0] != '-' && carg[1] != '-')
			return carg;
	}
	return NULL;
}

/*
 * option_matches function checks if an option is valid or not.
 * It returns the integer number corresponding to the option.
 */
int option_matches(char *arg)
{
        struct valid_options *opts;

        for (opts = &vopts[0]; opts->option; opts++) {
                if (!strcasecmp(opts->option, arg))
                        return opts->optval;
        }
	opt_inval = 1;
        return ERR;
}

/*
 * get_cmd_option function gets an option from commandline.
 */
int get_cmd_option(int argc, char *argv[])
{
	char *carg;

	if (optind < argc) {
		carg = argv[optind];
		if (carg[0] == '-' && carg[1] && '-' && carg[2]) {
			optind++;
			return option_matches(carg+2);
		}
	}
	opt_inval = 0;
	return ERR;
}

/*
 * checkRW function checks if a 'rw' option is valid and returns the corresponding integer number
 */
short checkRW(char *val)
{
	struct rw_value_pair *rw_vp;

	for (rw_vp = &rw_type[0]; rw_vp->rw; rw_vp++)
		if (!strcasecmp(rw_vp->rw, val))
			return rw_vp->value;

	return ERR;
}

/*
 * getRWStr function gets 'rw' string for the corresponding 'rw' integer value
 */
char *getRWStr(short val)
{
	struct rw_value_pair *rw_vp;

	for (rw_vp = &rw_type[0]; rw_vp->rw; rw_vp++)
		if (rw_vp->value == val)
			return rw_vp->rw;

	return "Unknown";
}

/*
 * checkDevice checks if a device exist and if the device is a valid block device.
 */
short checkDevice(char *device)
{
	struct stat devinfo;
	int ret = ERR;

	if (stat(device,&devinfo) < 0) {
		printf("'%s' - No such device exist\n",device);
		return ret;
	} 

	if (S_ISBLK(devinfo.st_mode)) {
		ret = SUCCESS;
	} else
		printf("'%s' is not a block device\n",device);
	
	return ret;
}

/*
 * checkPattern function checks if a pattern doesnt exceed more than 8 characters.
 */
short checkPattern(char *val)
{
	short ret = SUCCESS;

	if (strlen(val) > 8) {
		printf("Invalid pattern '%s'.\nPattern cannot be more than 8 characters.\n",val);
		ret = ERR;
	}
	return ret;
}

/*
 * checkTime function checks the runtime is valid or not.
 * It converts runtime in string format to runtime in number format.
 */
unsigned long checkTime(char *val)
{
	char *p , *b;
        int len;
	float rtime = 0;
	unsigned long mult = FALSE;
        unsigned long ret = FALSE;
	int alpha = 0;

        len = strlen(val) - 1;
        b = strdup(val);
        p = b;

        while ((p - b) < len) {
                if (!isdigit((int) *p) && (strncmp(p,".",1)))
                        break;
                p++;
        }

        if (!isalpha((int) *p))
                ret = FALSE;
	else {
        	if (!strcasecmp(p, "s"))
                	mult = 1000;
        	else if (!strcasecmp(p, "m"))
			mult = 60 * 1000;
		else if (!strcasecmp(p, "h"))
			mult = 60 * 60 * 1000;
		else if (!strcasecmp(p, "d"))
			mult = 24 * 60 * 60 * 1000;
		alpha = 1;
	}
	

	if (alpha)
		*p = '\0';

        rtime = atof(b);

	if ((mult == 0) && !alpha)
		mult = 1000;

        rtime *= mult;

        ret = rtime;

        return ret;
}

/*
 * ispowerof2 function checks if a value is power of 2 or not.
 * It returns true if value is power of 2. otherwisr returns false.
 */
bool ispowerof2(unsigned int x) {
   return x && !(x & (x - 1));
}

/*
 * checkBS function checks if the blocksize is valid or not.
 * It converts blocksize in string to a valid blocksize number.
 */
short checkBS(char *val)
{
	char *p , *b;
	int len, bs = -1, mult = 0;
	int ret = ERR;
	struct bs_order_pair *bs_op;

	len = strlen(val) - 1;
	b = strdup(val);
	p = b;

	while ((p - b) < len) {
		if (!isdigit((int) *p))
			break;
		p++;
	}

	if (!isalpha((int) *p))
		ret = ERR;

	if (!strcasecmp(p, "k")) {
		*p = '\0';
		mult = 1;
	}

	if (strncmp(p,".",1)) {
		bs = atoi(b);
		if (mult)
			bs *= 1024;	
	}

	for (bs_op = &bs_order[0]; bs_op->bs != -1; bs_op++) {
		if (bs_op->bs == bs) {
			ret = bs_op->order;
			break;
		}
	}
	
	return ret;	
}

/*
 * strip_blank_front function removes space in the beginning of the line
 */
void strip_blank_front(char **p)
{
        char *s = *p;

        if (!strlen(s))
                return;
        while (isspace((int) *s))
                s++;

        *p = s;
}

/*
 * strip_blank_end function removes control characters at the end of line.
 */
void strip_blank_end(char *p)
{
        char *start = p, *s;

        if (!strlen(p))
                return;

        s = strchr(p, ';');
        if (s)
                *s = '\0';
        s = strchr(p, '#');
        if (s)
                *s = '\0';
        if (s)
                p = s;

        s = p + strlen(p);
        while ((isspace((int) *s) || iscntrl((int) *s)) && (s > start))
                s--;

        *(s + 1) = '\0';
}

/*
 * is_empty_or_comment checks if a line is empty or not
 */
static int is_empty_or_comment(char *line)
{
        unsigned int i;

        for (i = 0; i < strlen(line); i++) {
                if (line[i] == ';')
                        return 1;
                if (line[i] == '#')
                        return 1;
                if (!isspace((int) line[i]) && !iscntrl((int) line[i]))
                        return 0;
        }

        return 1;
}

/*
 * parse_line gets option and returns value for the option in 'value' filed
 */
int parse_line(char **opts, char **input, char **value)
{
        char *ret;
        int retval = ERR;

        ret = strchr(*opts, '=');
        if (ret) {
                *value = ret;
                *ret = '\0';
                ret = *opts;
                (*value)++;
                strip_blank_front(&(*value));
                strip_blank_end(ret);
                *opts = ret;
                retval = SUCCESS;
        }

        return retval;
}
	
/*
 * parse_And_fill_workload function parses every line for a single workload and fills user_data structure based on the parsed values
 */
int parse_and_fill_workload(char **input, int num_opts, char *name)
{
	int i = 0;
	char *value;
	int ret = ERR;
	char **opts;
	int match = ERR;
	int retval = ERR;
	int k = 0;
	int is_present = 1;

	opts = malloc(sizeof(char *) * num_opts);

	for (i = 0; i < num_opts; i++) {
		opts[i] = strdup(input[i]);
		ret = parse_line(&opts[i], &input[i], &value);
	
		if (ret == ERR)
			goto out;

		match = option_matches(opts[i]);
		if (match == ERR) {
			ret = ERR;
			printf("Unrecognized option '%s'\n",opts[i]);
			goto out;
		}

		retval = checkValue(match, value);
		if (retval == ERR) {
			ret = retval;
			goto out;
		}
		k++;	
	}

	if (dev_present)
		dev_present = 0;
	else {
		printf("'device' option is required for workload section '%s'\n",name);
		ret = ERR;
		is_present = 0;
	}

	out:
		for (i = 0; i < num_opts; i++) {
			if (opts[i])
				free(opts[i]);
		}
		free(opts);

		if ((ret == ERR) && is_present)
			printf("\nkiotool : failed while parsing line '%s'\n",input[k]);

	return ret;
}

/*
 * parse_file function parses file and gets workload configuration.
 */
int parse_file(int argc, char *argv[])
{
	char *file, *p;
	FILE *fp;
	int skip_get = 0;
	char **opts;
	int ret = ERR;
	int num_opts, alloc_opts, i;
	char name[280];
	char string[4096];

	if (argc != 2) {
		printf("%s\n",STR_INVALID_ARGUMENTS);
		show_usage();
		return ERR;
	}
	file = argv[optind];

	fp = fopen(file, "r");
	if(!fp) {
		printf("Workload file '%s' does not exist.\n",file); 
		return ERR;
	}

	alloc_opts = 5;
	opts = malloc(sizeof(char *) * alloc_opts);
	num_opts = 0;

	do {
		if (!skip_get) {
			p = fgets(string, 4096, fp);
			if (!p)
				break;
		}
		
		skip_get = 0;
		strip_blank_front(&p);
		strip_blank_end(p);
		
		if (is_empty_or_comment(p))
			continue;
	
		if (sscanf(p, "[%255[^\n]]", name) != 1) {
			printf("kiotool : failed while parsing line '%s'\n",p);
			ret = ERR;
			break;
		}
		name[strlen(name) - 1] = '\0';
		num_opts = 0;
		memset(opts, 0, alloc_opts * sizeof(char *));

		while (1) {
			p = fgets(string, 4096, fp);
	                if (!p)
        	        	break;
			
			if (is_empty_or_comment(p))
				continue;

			strip_blank_front(&p);

			if (p[0] == '[') {
				skip_get = 1;
					
				if (devindex < MAX_DEVICE_CNT)
					devindex++;
				else {
					ret = ERR;
					goto out;
				}
				break;
			}
	
			strip_blank_end(p);

			if (num_opts == alloc_opts) {
				alloc_opts <<= 1;
				opts = realloc(opts, alloc_opts * sizeof(char *));
			}

			opts[num_opts] = strdup(p);
			num_opts++;
		}

		ret = parse_and_fill_workload(opts, num_opts, name);

		if (!ret) {
			ddata.devcount = totalcnt;
			work_data++;
			totalcnt++;
		}  
	
		out:	
		for (i = 0; i < num_opts; i++)
			free(opts[i]);
			
		num_opts = 0;
	
	} while(!ret);

	free(opts);
	fclose(fp);	
	
	return ret;
}

/* 
 * checkValue function checks if the options are valid or not.
 */
int checkValue(int opt, char *val)
{
	int ret = SUCCESS;
	short oval;
	unsigned long rt;

	switch(opt) {
	case 'd':
		dev_present = 1;
		oval = checkDevice(val);
		
		if (oval == ERR)
			ret = ERR;
		else
			strncpy(work_data->devicename, val, strlen(val));
		break;
	case 't':
		rt = checkTime(val);
		if ((rt > MAX_TIME_MSECS) || (rt == 0)) {
			printf("Invalid runtime '%s'\n",val);
			ret = ERR;
		} else
			work_data->runtime = rt;
		break;
	case 'p':
		if (checkPattern(val) < 0)
			ret = ERR;
		else {
			work_data->pattern_based = 1;
			strncpy(work_data->pattern, val, strlen(val));
		}
		break;
	case 'm':
		if((oval = checkRW(val)) < 0) {
			printf("Invalid rw option '%s'\n",val);
			ret = ERR;
		} else
			work_data->rw = oval;
		break;
	case 'b':
		if ((oval = checkBS(val)) < 0) {
			printf("Invalid block size '%s'\n",val);
			ret = ERR;
		} else
			work_data->bs = oval;
		break;
	case 'v':
		if (!strcmp(val, "1")) {
			printf("comes to 1\n");
			work_data->check_crc = 1;
		}
		else if (!strcmp(val, "0")) {
			printf("comes to zero\n");
			work_data->check_crc = 0;
		}
		else {
			printf("comes to error\n");
			ret = ERR;
		}
		break;
	default:
		ret = ERR;
	}
	
	return ret;
}

/*
 * parse_cmd_line function parses command line arguments.
 * It decides whether workload is specified in command line or in workload file.
 * It returns if workload options are specified in a workload file. otherwise continues execution.
 */
int parse_cmd_line(int argc, char *argv[])
{
	int ret = TRUE;
	char *val;
	int retval = ERR;

	if (argc < 2) {
		printf(STR_NO_WORK_LOAD_DEFINED);
		printf("\n");
		show_usage();
		return ERR;
	}

	optind = 1;
	while ((ret = get_cmd_option(argc,argv)) != ERR) {
		val = get_optval(argc, argv);

		if (!val) {
			printf("Option '%s' requires an argument\n",argv[optind - 1]);
			ret = ERR;
			break;
		}
		optind++;

		retval = checkValue(ret, val);
		if (retval == ERR) {
			ret = ERR;
			break;
		}
			
		if (optind == argc)
			return SUCCESS;
	}

	if (optind == 1)
		ret = TRUE;
	else if (opt_inval >= 0)
		printf("Unrecognised option '%s'\n",argv[optind - opt_inval]);
	
	return ret; 
}

/*
 * eta_to_str function converts seconds to equivalent readable string
 */
void eta_to_str(char *str, unsigned long eta_sec)
{
        unsigned int d, h, m, s;
        int disp_hour = 0;

        s = eta_sec % 60;
        eta_sec /= 60;
        m = eta_sec % 60;
        eta_sec /= 60;
        h = eta_sec % 24;
        eta_sec /= 24;
        d = eta_sec;

        if (d) {
                disp_hour = 1;
                str += sprintf(str, "%02ud:", d);
        }

        if (h || disp_hour)
                str += sprintf(str, "%02uh:", h);

        str += sprintf(str, "%02um:", m);
        str += sprintf(str, "%02us", s);
}

/* 
 * update_eta function prints elapsed time and workload details on the console
 */
void *update_eta()
{
	unsigned long rtime;
	char s[125];
	char r[125];

	rtime = totaltime / 1000;

	eta_to_str(s,rtime);
	strncpy(r,s,strlen(s));
	
	printf("\n\nStarting IO generation\n");
	while (rtime) {
		eta_to_str(s,rtime);
		printf("Total time : %s		[eta %s",r,s);
		sleep(1);
		printf("]\n\033[F\033[J");
		rtime--;
	}
	eta_to_str(s,0);
	printf("Total time : %s         [eta %s]\n",r,s);
	printf("\n finished execution in pthread\n");	

	while (done == 0) {
		printf("ioctl response has not arrived. lets wait for sometime\n");
		sleep(1);
	}
	return NULL;
}
	
int main(int argc, char *argv[])
{
	int ret;
	int i;
	struct user_data *udt;
	int tc;
	pthread_t update_time;
	int fd = 0;
	
	init_user_data();
        
	if ((ret = parse_cmd_line(argc, argv)) < 0)
		return ERR;
	else if (ret) {
		if (parse_file(argc, argv) < 0) {
			return ERR;
		}
	} 

	udt = ddata.udata;
	tc = ddata.devcount;

	totaltime = udt->runtime;

	for (i = 0; i < tc ; i++) {
		printf("Workload %d : on device [%s] : bs=%d , rw=%s\n",i,udt->devicename,udt->bs,getRWStr(udt->rw));
		if (totaltime < udt->runtime)
			totaltime = udt->runtime;
		udt++;
	}
	pthread_create(&update_time, NULL, update_eta, NULL);
	printf("our thread is created\n");
	fd = open("/dev/kio", O_RDWR);
	if (fd < 0) {
		printf("kio misc not found\n");
		return -1;
	}
	errno = 0;
	printf("going to send ioctl\n");
	ret = ioctl(fd,IO_LOAD,&ddata);
	done = 1;	
	if ((ret != 0) && errno) {
		printf("some error has occured when giving ioctl\n");
		
		if (errno == EAGAIN) {
			printf("try again after sometime\n");
		}
		return ret;
	} else
		printf("success success success\n");
	printf("got the response so going to join thread\n");
	//now data is filled. call ioctl
	pthread_join(update_time, NULL);
	printf("joined. so bye bye\n");

	return 0;
}
