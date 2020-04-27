


#define	FUSE_USE_VERSION	31

#include <fuse3/cuse_lowlevel.h>
#include <fuse3/fuse_opt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/uio.h>
#include <sys/ioctl.h>

struct zfsvfs;
struct zfsdev_state;

typedef struct zfsvfs zfsvfs_t;
typedef struct zfsdev_state zfsdev_state_t;

#include <sys/zfs_context.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_ioctl_impl.h>

void zfs_ioctl_init(void);

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


static void zsk_open(fuse_req_t req, struct fuse_file_info *fi)
{
	fuse_reply_open(req, fi);
}

void
zsk_release(fuse_req_t req, struct fuse_file_info *fi)
{
	fuse_reply_err(req, 0);
}

typedef struct zsk_cmd_save {
	uint64_t	zc_nvlist_src;		/* really (char *) */
	uint64_t	zc_nvlist_dst;		/* really (char *) */
	uint64_t	zc_history;		/* really (char *) */
	uint64_t	zc_nvlist_conf;		/* really (char *) */
} zsk_cmd_save_t;

#define	MAX_IN_VEC	4
#define	MAX_OUT_VEC	9
#define	MAX_IOV_LEN	32768
#define	MAX_IOV_BYTES	(131072 - sizeof (zfs_cmd_t))

static void zsk_ioctl(fuse_req_t req, int cmd, void *arg,
    struct fuse_file_info *fi, unsigned flags, const void *in_buf,
    size_t in_bufsz, size_t out_bufsz)
{
	(void) fi;
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
	in_iov[in_vec_cnt++].iov_len = sizeof (zfs_cmd_t);

	/* where output zfs_cmd_t is going <-- */
	out_iov[out_vec_cnt].iov_base = arg;
	out_iov[out_vec_cnt++].iov_len = sizeof (zfs_cmd_t);

	/* read in zfs_cmd_t */
	if (!in_bufsz) {
		/* Retry this ioctl call with the zfs command data filled in */
		(void) fuse_reply_ioctl_retry(req, in_iov, 1, NULL, 0);

		fprintf(stderr, "\n================= 0x%x =================\n",
		    cmd);
		fprintf(stderr, "copied %d byte zfs_cmd_t\n",
		    (int)in_iov[0].iov_len);
		return;
	}

	switch(cmd) {
	case ZFS_IOC_POOL_CREATE:
	case ZFS_IOC_CREATE:
		fprintf(stderr, "missing ZPL/znode layer\n");
		/* fall through */

	case ZFS_IOC_RECV:
	case ZFS_IOC_SEND:
		/* fall through */

		fuse_reply_err(req, ENOTSUP);
		return;
	}

	fprintf(stderr, "0x%x\n", cmd);
	fprintf(stderr, "arg: %p\n", arg);
	fprintf(stderr, "in_bufsz: %d\n", (int)in_bufsz);
	fprintf(stderr, "out_bufsz: %d\n", (int)out_bufsz);

	zc = (zfs_cmd_t *)in_buf;

	/* read in the nvlist and history and prep for output */
	if (in_bufsz == sizeof (zfs_cmd_t) && out_bufsz == 0) {
		/* read in optional config nvlist */
		if (zc->zc_nvlist_conf_size > 0) {
			fprintf(stderr, "\tzc_nvlist_conf: 0x%llx, "
			    "zc_nvlist_conf_size: %d\n",
			    (long long unsigned)zc->zc_nvlist_conf,
			    (int)zc->zc_nvlist_conf_size);
			in_iov[in_vec_cnt].iov_base =
			    (void *)zc->zc_nvlist_conf;
			in_iov[in_vec_cnt++].iov_len = zc->zc_nvlist_conf_size;
		}

		/* read in optional source nvlist */
		if (zc->zc_nvlist_src_size > 0) {
			fprintf(stderr, "\tzc_nvlist_src: 0x%llx, "
			    "zc_nvlist_src_size: %d\n",
			    (long long unsigned)zc->zc_nvlist_src,
			    (int)zc->zc_nvlist_src_size);
			in_iov[in_vec_cnt].iov_base = (void *)zc->zc_nvlist_src;
			in_iov[in_vec_cnt++].iov_len = zc->zc_nvlist_src_size;
		}

		/* read in optional history (zpool destroy | zpool export) */
		if (zc->zc_history && zc->zc_history_len == 0) {
			fprintf(stderr, "\tzc_history: 0x%llx, "
			    "zc_history_len: %d, zc_history_offset: %d\n",
			    (long long unsigned)zc->zc_history,
			    (int)zc->zc_history_len,
			    (int)zc->zc_history_offset);
			in_iov[in_vec_cnt].iov_base = (void *)zc->zc_history;
			in_iov[in_vec_cnt++].iov_len = HIS_MAX_RECORD_LEN;
		}

		/* prepare output buffers (for copyout) */
		if (zc->zc_nvlist_dst_size > 0 ||
		    cmd == ZFS_IOC_POOL_GET_HISTORY) {
			char *iovbase;
			int length;

			if (zc->zc_nvlist_dst_size) {
				length =
				    MIN(MAX_IOV_BYTES, zc->zc_nvlist_dst_size);
				iovbase = (void *)zc->zc_nvlist_dst;
			} else {
				length = MIN(MAX_IOV_BYTES, zc->zc_history_len);
				iovbase = (void *)zc->zc_history;
			}
			fprintf(stderr, "\toutput buffer: %p, "
			    "output size: %d\n", iovbase, length);

			/*
			 * The maximum iov_len seems to be 32K so we
			 * need to break up large nvlist dst output
			 * over multiple vectors
			 */
			while (length > 0 && out_vec_cnt < MAX_OUT_VEC) {
				int iovlen =
				    length > MAX_IOV_LEN ? MAX_IOV_LEN : length;

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
	ret_iov[ret_vec_cnt++].iov_len = sizeof (zfs_cmd_t);

	/* NOTE -- assumes that we have only one of nvlist_dst and history */
	if (zc->zc_nvlist_dst_size > 0 || zc->zc_history_len > 0) {
		char *buffer;
		int length;

		if (zc->zc_nvlist_dst_size) {
			length = MIN(MAX_IOV_BYTES, zc->zc_nvlist_dst_size);
			buffer = malloc(length);
			zc->zc_nvlist_dst = (uint64_t)(uintptr_t)buffer;
			zc->zc_nvlist_dst_size = length;
		} else {
			length = MIN(MAX_IOV_BYTES, zc->zc_history_len);
			buffer = malloc(length);
			zc->zc_history = (uint64_t)(uintptr_t)buffer;
			zc->zc_history_len = length;
		}

		/*
		 * The maximum iov_len seems to be 32K so we need to break
		 * up a large output buffer over multiple vectors
		 */
		while (length > 0 && ret_vec_cnt < MAX_OUT_VEC) {
			int iovlen =
			    length > MAX_IOV_LEN ? MAX_IOV_LEN : length;

			ret_iov[ret_vec_cnt].iov_base = buffer;
			ret_iov[ret_vec_cnt++].iov_len = iovlen;
			length -= iovlen;
			buffer += iovlen;
		}
	}

	/* fix up the nvlist pointers to reference the in_buf locations */
	extra = (char *)in_buf + sizeof (zfs_cmd_t);
	if (zc->zc_nvlist_conf_size > 0) {
		zc->zc_nvlist_conf = (uint64_t)(uintptr_t)extra;
		extra += zc->zc_nvlist_conf_size;
		fprintf(stderr, "\t patched zc_nvlist_conf: 0x%llx,"
		    "zc_nvlist_conf_size: %d\n",
		    (long long unsigned)zc->zc_nvlist_conf,
		    (int)zc->zc_nvlist_conf_size);
	}
	if (zc->zc_nvlist_src_size > 0) {
		zc->zc_nvlist_src = (uint64_t)(uintptr_t)extra;
		extra += zc->zc_nvlist_src_size;
	}
	if (zc->zc_history && zc->zc_history_len == 0) {
		printf("last history: %s\n", extra);
		zc->zc_history = (uint64_t)(uintptr_t)extra;
		extra += HIS_MAX_RECORD_LEN;
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

#define	CUSEXMP_OPT(t, p) { t, offsetof(struct cusexmp_param, p), 1 }

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

	(void) outargs;
	(void) arg;

	switch (key) {
	case 0:
		param->is_help = 1;
		fprintf(stderr, "%s", usage);
		return (fuse_opt_add_arg(outargs, "-ho"));
	default:
		return (1);
	}
}

static const struct cuse_lowlevel_ops zsk_dev_ops = {
	.init_done	= zsk_init_done,
	.destroy	= zsk_destroy,
	.open		= zsk_open,
	.release	= zsk_release,
	.ioctl		= zsk_ioctl
};

int
main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct cusexmp_param param = { 0, 0, NULL, 0 };
	const char *dev_info_argv[] = { "DEVNAME=zfs" };
	struct cuse_info ci;

	if (fuse_opt_parse(&args, &param, cusexmp_opts, cusexmp_process_arg)) {
		printf("failed to parse option\n");
		return (1);
	}
	if (access("/dev/zfs", F_OK) != -1) {
		(void) fprintf(stderr, "'%s' control node already exists!\n",
		    "/dev/zfs");
		return (1);
	}

	memset(&ci, 0, sizeof (ci));
	ci.dev_major = param.major;
	ci.dev_minor = param.minor;
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	return cuse_lowlevel_main(args.argc, args.argv, &ci, &zsk_dev_ops,
	    "private data goes here");
}
