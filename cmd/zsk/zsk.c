/*
  CUSE example: Character device in Userspace
  Copyright (C) 2008-2009  SUSE Linux Products GmbH
  Copyright (C) 2008-2009  Tejun Heo <tj@kernel.org>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

*/

/** @file
 *
 * This example demonstrates how to implement a character device in
 * userspace ("CUSE"). This is only allowed for root. The character
 * device should appear in /dev under the specified name. It can be
 * tested with the cuse_client.c program.
 *
 * Mount the file system with:
 *
 *     cuse -f --name=mydevice
 *
 * You should now have a new /dev/mydevice character device. To "unmount" it,
 * kill the "cuse" process.
 *
 * To compile this example, run
 *
 *     gcc -Wall cuse.c `pkg-config fuse3 --cflags --libs` -o cuse
 *
 * ## Source code ##
 * \include cuse.c
 */


#define FUSE_USE_VERSION 31

#include <fuse3/cuse_lowlevel.h>
#include <fuse3/fuse_opt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "ioctl.h"

struct zfsvfs;
struct zfsdev_state;

typedef struct zfsvfs zfsvfs_t;
typedef struct zfsdev_state zfsdev_state_t;

#include <sys/zfs_context.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_ioctl_impl.h>

void zfs_ioctl_init(void);

static void *cusexmp_buf;
static size_t cusexmp_size;

static const char *usage =
"usage: zsk [options]\n"
"\n"
"options:\n"
"    --help|-h             print this help message\n"
"    --maj=MAJ|-M MAJ      device major number\n"
"    --min=MIN|-m MIN      device minor number\n"
"    -d   -o debug         enable debug output (implies -f)\n"
"    -f                    foreground operation\n"
"    -s                    disable multi-threaded operation\n"
"\n";

static int cusexmp_resize(size_t new_size)
{
	void *new_buf;

	if (new_size == cusexmp_size)
		return 0;

	new_buf = realloc(cusexmp_buf, new_size);
	if (!new_buf && new_size)
		return -ENOMEM;

	if (new_size > cusexmp_size)
		memset(new_buf + cusexmp_size, 0, new_size - cusexmp_size);

	cusexmp_buf = new_buf;
	cusexmp_size = new_size;

	return 0;
}

static int cusexmp_expand(size_t new_size)
{
	if (new_size > cusexmp_size)
		return cusexmp_resize(new_size);
	return 0;
}

static void zsk_open(fuse_req_t req, struct fuse_file_info *fi)
{
	fuse_reply_open(req, fi);
}

static void zsk_read(fuse_req_t req, size_t size, off_t off,
			 struct fuse_file_info *fi)
{
	(void)fi;

	if (off >= cusexmp_size)
		off = cusexmp_size;
	if (size > cusexmp_size - off)
		size = cusexmp_size - off;

	fuse_reply_buf(req, cusexmp_buf + off, size);
}

static void zsk_write(fuse_req_t req, const char *buf, size_t size,
			  off_t off, struct fuse_file_info *fi)
{
	(void)fi;

	if (cusexmp_expand(off + size)) {
		fuse_reply_err(req, ENOMEM);
		return;
	}

	memcpy(cusexmp_buf + off, buf, size);
	fuse_reply_write(req, size);
}

static void fioc_do_rw(fuse_req_t req, void *addr, const void *in_buf,
		       size_t in_bufsz, size_t out_bufsz, int is_read)
{
	const struct fioc_rw_arg *arg;
	struct iovec in_iov[2], out_iov[3], iov[3];
	size_t cur_size;

	/* read in arg */
	in_iov[0].iov_base = addr;
	in_iov[0].iov_len = sizeof(*arg);
	if (!in_bufsz) {
		fuse_reply_ioctl_retry(req, in_iov, 1, NULL, 0);
		return;
	}
	arg = in_buf;
	in_buf += sizeof(*arg);
	in_bufsz -= sizeof(*arg);

	/* prepare size outputs */
	out_iov[0].iov_base =
		addr + offsetof(struct fioc_rw_arg, prev_size);
	out_iov[0].iov_len = sizeof(arg->prev_size);

	out_iov[1].iov_base =
		addr + offsetof(struct fioc_rw_arg, new_size);
	out_iov[1].iov_len = sizeof(arg->new_size);

	/* prepare client buf */
	if (is_read) {
		out_iov[2].iov_base = arg->buf;
		out_iov[2].iov_len = arg->size;
		if (!out_bufsz) {
			fuse_reply_ioctl_retry(req, in_iov, 1, out_iov, 3);
			return;
		}
	} else {
		in_iov[1].iov_base = arg->buf;
		in_iov[1].iov_len = arg->size;
		if (arg->size && !in_bufsz) {
			fuse_reply_ioctl_retry(req, in_iov, 2, out_iov, 2);
			return;
		}
	}

	/* we're all set */
	cur_size = cusexmp_size;
	iov[0].iov_base = &cur_size;
	iov[0].iov_len = sizeof(cur_size);

	iov[1].iov_base = &cusexmp_size;
	iov[1].iov_len = sizeof(cusexmp_size);

	if (is_read) {
		size_t off = arg->offset;
		size_t size = arg->size;

		if (off >= cusexmp_size)
			off = cusexmp_size;
		if (size > cusexmp_size - off)
			size = cusexmp_size - off;

		iov[2].iov_base = cusexmp_buf + off;
		iov[2].iov_len = size;
		fuse_reply_ioctl_iov(req, size, iov, 3);
	} else {
		if (cusexmp_expand(arg->offset + in_bufsz)) {
			fuse_reply_err(req, ENOMEM);
			return;
		}

		memcpy(cusexmp_buf + arg->offset, in_buf, in_bufsz);
		fuse_reply_ioctl_iov(req, in_bufsz, iov, 2);
	}
}


typedef struct zsk_cmd_save {
	uint64_t	zc_nvlist_src;		/* really (char *) */
	uint64_t	zc_nvlist_dst;		/* really (char *) */
	uint64_t	zc_history;		/* really (char *) */
	uint64_t	zc_nvlist_conf;		/* really (char *) */
} zsk_cmd_save_t;

#define	MAX_IN_VEC	3
#define	MAX_OUT_VEC	9
#define	MAX_IOV_LEN	32768
#define	MAX_IOV_BYTES	(131072 - sizeof(zfs_cmd_t))

static void zsk_ioctl_zfs(fuse_req_t req, int cmd, void *arg,
    const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	zfs_cmd_t *zc;
	const char *extra;
	zsk_cmd_save_t saved;
	struct iovec in_iov[MAX_IN_VEC];
	struct iovec out_iov[MAX_OUT_VEC];
	struct iovec ret_iov[MAX_OUT_VEC];
	int in_vec_cnt = 0;
	int out_vec_cnt = 0;
	int ret_vec_cnt = 0;
	uint_t vecnum;
	int error;

	/* where input zfs_cmd_t is coming from --> */
	in_iov[in_vec_cnt].iov_base = arg;
	in_iov[in_vec_cnt++].iov_len = sizeof(zfs_cmd_t);

	/* where output zfs_cmd_t is going <-- */
	out_iov[out_vec_cnt].iov_base = arg;
	out_iov[out_vec_cnt++].iov_len = sizeof(zfs_cmd_t);

	fprintf(stderr, "arg: %p\n", arg);
	fprintf(stderr, "in_bufsz: %d\n", (int)in_bufsz);
	fprintf(stderr, "out_bufsz: %d\n", (int)out_bufsz);

	/* read in zfs_cmd_t */
	if (!in_bufsz) {
		/* Retry this ioctl call with the zfs command data filled in */
		(void) fuse_reply_ioctl_retry(req, in_iov, 1, NULL, 0);

		fprintf(stderr, "copied %d byte zfs_cmd_t\n", (int)in_iov[0].iov_len);
		return;
	}

	zc = (zfs_cmd_t *)in_buf;

	if (in_bufsz == sizeof(zfs_cmd_t) && out_bufsz == 0) {
		/* read in optional config nvlist */
		if (zc->zc_nvlist_conf_size > 0) {
			fprintf(stderr, "\tzc_nvlist_conf: 0x%llx, zc_nvlist_conf_size: %d\n",
			    (long long unsigned)zc->zc_nvlist_conf,
			    (int)zc->zc_nvlist_conf_size);
			in_iov[in_vec_cnt].iov_base = (void *)zc->zc_nvlist_conf;
			in_iov[in_vec_cnt++].iov_len = zc->zc_nvlist_conf_size;
		}

		/* read in optional source nvlist */
		if (zc->zc_nvlist_src_size > 0) {
			fprintf(stderr, "\tzc_nvlist_src: 0x%llx, zc_nvlist_src_size: %d\n",
			    (long long unsigned)zc->zc_nvlist_src,
			    (int)zc->zc_nvlist_src_size);
			in_iov[in_vec_cnt].iov_base = (void *)zc->zc_nvlist_src;
			in_iov[in_vec_cnt++].iov_len = zc->zc_nvlist_src_size;
		}

		/* prepare destination nvlist buffer */
		if (zc->zc_nvlist_dst_size > 0) {
			fprintf(stderr, "\tzc_nvlist_dst: 0x%llx, zc_nvlist_dst_size: %d\n",
			    (long long unsigned)zc->zc_nvlist_dst,
			    (int)zc->zc_nvlist_dst_size);

			int length = MIN(MAX_IOV_BYTES, zc->zc_nvlist_dst_size);
			char *iovbase = (void *)zc->zc_nvlist_dst;

			/*
			 * The maximum iov_len seems to be 32K so we
			 * need to break up large nvlist dst output
			 * over multiple vectors
			 */
			while (length > 0 && out_vec_cnt < MAX_OUT_VEC) {
				int iovlen =
				    length > MAX_IOV_LEN ? MAX_IOV_LEN : length;

				printf("out_iov[%d] = %d\n", out_vec_cnt, iovlen);

				out_iov[out_vec_cnt].iov_base = iovbase;
				out_iov[out_vec_cnt++].iov_len = iovlen;
				length -= iovlen;
				iovbase += iovlen;
			}
		}

		/*
		 *       in_buf                   out_buf
		 * +----------------+        +----------------+
		 * |                |        |                |
		 * |     copy of    |        |     copy of    |
		 * |    zio_cmd_t   |        |    zio_cmd_t   |
		 * |                |        |                |
		 * +----------------+        +----------------+
		 * |     copy of    |        |     copy of    |
		 * | zc_nvlist_conf |        | zc_nvlist_dst  |
		 * +----------------+        |                |
		 * |     copy of    |        +----------------+
		 * | zc_nvlist_src  |
		 * +----------------+
		 */

		/* Retry this ioctl call with the nvlist data filled in */
		error = fuse_reply_ioctl_retry(req, in_iov, in_vec_cnt,
		    out_iov, out_vec_cnt);

		fprintf(stderr, "copied nvl lists and set output (%d)\n", error);

		return;
	}

	/*
	 * Command data is now ready
	 */

	saved.zc_nvlist_conf = zc->zc_nvlist_conf;
	saved.zc_nvlist_src = zc->zc_nvlist_src;
	saved.zc_nvlist_dst = zc->zc_nvlist_dst;
	saved.zc_history = zc->zc_history;

	ret_iov[ret_vec_cnt].iov_base = zc;
	ret_iov[ret_vec_cnt++].iov_len = sizeof(zfs_cmd_t);

	if (zc->zc_nvlist_dst_size > 0) {
		int length = MIN(MAX_IOV_BYTES, zc->zc_nvlist_dst_size);
		char *iovbase = malloc(zc->zc_nvlist_dst_size);

		/*
		 * The maximum iov_len seems to be 32K so we
		 * need to break up large nvlist dst output
		 * over multiple vectors
		 */
		while (length > 0 && ret_vec_cnt < MAX_OUT_VEC) {
			int iovlen = length > MAX_IOV_LEN ? MAX_IOV_LEN : length;

			printf("ret_iov[%d] = %d\n", ret_vec_cnt, iovlen);

			ret_iov[ret_vec_cnt].iov_base = iovbase;
			ret_iov[ret_vec_cnt++].iov_len = iovlen;
			length -= iovlen;
			iovbase += iovlen;
		}
		zc->zc_nvlist_dst = (uint64_t)(uintptr_t)ret_iov[1].iov_base;
	}

	/* fix up the nvlist pointers to reference the in_buf locations */
	extra = (char *)in_buf + sizeof(zfs_cmd_t);
	if (zc->zc_nvlist_conf_size > 0 ) {
		zc->zc_nvlist_conf = (uint64_t)(uintptr_t)extra;
		extra += zc->zc_nvlist_conf_size;
		fprintf(stderr, "\t patched zc_nvlist_conf: 0x%llx, zc_nvlist_conf_size: %d\n",
		    (long long unsigned)zc->zc_nvlist_conf,
		    (int)zc->zc_nvlist_conf_size);
	}
	if (zc->zc_nvlist_src_size > 0 ) {
		zc->zc_nvlist_src = (uint64_t)(uintptr_t)extra;
		extra += zc->zc_nvlist_src_size;
	}

	if (zc->zc_history != 0) {
		fprintf(stderr, "\tzc_history: 0x%llxp\n",
		    (long long unsigned)zc->zc_history);
	}

	vecnum = cmd - ZFS_IOC_FIRST;
	error = zfsdev_ioctl_common(vecnum, zc);

	zc->zc_nvlist_conf = saved.zc_nvlist_conf;
	zc->zc_nvlist_src = saved.zc_nvlist_src;
	zc->zc_nvlist_dst = saved.zc_nvlist_dst;
	zc->zc_history = saved.zc_history;

	fprintf(stderr, "zsk ioct %d, '%s', err %d, filled %d, %d bytes\n",
	    vecnum, zc->zc_name, error, (int)zc->zc_nvlist_dst_filled,
	    (int)zc->zc_nvlist_dst_size);

	(void) fuse_reply_ioctl_iov(req, error, ret_iov, ret_vec_cnt);

	if (ret_vec_cnt > 1) {
		free(ret_iov[1].iov_base);
	}
}

static void zsk_ioctl(fuse_req_t req, int cmd, void *arg,
    struct fuse_file_info *fi, unsigned flags, const void *in_buf,
    size_t in_bufsz, size_t out_bufsz)
{
	int is_read = 0;

	(void)fi;

	if (flags & FUSE_IOCTL_COMPAT) {
		fuse_reply_err(req, ENOSYS);
		return;
	}

	switch (cmd) {
	case FIOC_GET_SIZE:
		if (!out_bufsz) {
			struct iovec iov = { arg, sizeof(size_t) };

			fuse_reply_ioctl_retry(req, NULL, 0, &iov, 1);
		} else
			fuse_reply_ioctl(req, 0, &cusexmp_size,
					 sizeof(cusexmp_size));
		break;

	case FIOC_SET_SIZE:
		if (!in_bufsz) {
			struct iovec iov = { arg, sizeof(size_t) };

			fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
		} else {
			cusexmp_resize(*(size_t *)in_buf);
			fuse_reply_ioctl(req, 0, NULL, 0);
		}
		break;

	case FIOC_READ:
		is_read = 1;
		/* fall through */
	case FIOC_WRITE:
		fioc_do_rw(req, arg, in_buf, in_bufsz, out_bufsz, is_read);
		break;

	default:
		zsk_ioctl_zfs(req, cmd, arg, in_buf, in_bufsz, out_bufsz);
	}
}
/*
 * called after initialization is complete
 */
static void zsk_init_done(void *userdata)
{
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	zfs_ioctl_init();
}

/*
 * Cleanup on termination
 */
static void zsk_destroy(void *userdata)
{
	kernel_fini();
}

struct cusexmp_param {
	unsigned		major;
	unsigned		minor;
	char			*dev_name;
	int			is_help;
};

#define CUSEXMP_OPT(t, p) { t, offsetof(struct cusexmp_param, p), 1 }

static const struct fuse_opt cusexmp_opts[] = {
	CUSEXMP_OPT("-M %u",		major),
	CUSEXMP_OPT("--maj=%u",		major),
	CUSEXMP_OPT("-m %u",		minor),
	CUSEXMP_OPT("--min=%u",		minor),
	CUSEXMP_OPT("-n %s",		dev_name),
	CUSEXMP_OPT("--name=%s",	dev_name),
	FUSE_OPT_KEY("-h",		0),
	FUSE_OPT_KEY("--help",		0),
	FUSE_OPT_END
};

static int cusexmp_process_arg(void *data, const char *arg, int key,
			       struct fuse_args *outargs)
{
	struct cusexmp_param *param = data;

	(void)outargs;
	(void)arg;

	switch (key) {
	case 0:
		param->is_help = 1;
		fprintf(stderr, "%s", usage);
		return fuse_opt_add_arg(outargs, "-ho");
	default:
		return 1;
	}
}

static const struct cuse_lowlevel_ops zsk_ops = {
	.open		= zsk_open,
	.read		= zsk_read,
	.write		= zsk_write,
	.ioctl		= zsk_ioctl,
	.init_done	= zsk_init_done,
	.destroy	= zsk_destroy
};

int main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct cusexmp_param param = { 0, 0, NULL, 0 };
	const char *dev_info_argv[] = { "DEVNAME=zfs" };
	struct cuse_info ci;

	if (fuse_opt_parse(&args, &param, cusexmp_opts, cusexmp_process_arg)) {
		printf("failed to parse option\n");
		return 1;
	}
	if( access("/dev/zfs", F_OK ) != -1 ) {
		(void)fprintf(stderr, "'%s' control node already exists!\n", "/dev/zfs");
		return 1;
	}

	memset(&ci, 0, sizeof(ci));
	ci.dev_major = param.major;
	ci.dev_minor = param.minor;
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	return cuse_lowlevel_main(args.argc, args.argv, &ci, &zsk_ops, "private data goes here");
}
