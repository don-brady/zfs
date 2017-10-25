/*
 * ztour -- tour zfs file system metadata as a userland file system
 */

#define	FUSE_USE_VERSION	30

#include <sys/types.h>
#include <sys/sysmacros.h>

#include <fuse/fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include "zt_util.h" 


char *vn_dumpdir = NULL;


enum root_entry_ids {
	ROOT_DIR_MOS_ID = 4,
	ROOT_DIR_CONFIG_ID = 5
};

typedef struct ztour_dirent {
	ino_t	ztd_ino;
	off_t	ztd_position;
	mode_t	ztd_type;	/* DT_DIR, DT_LNK, and DT_REG */
	char	ztd_name[128];	/* filename */
} ztour_dirent_t;

typedef struct root_direntry {
	char		*rde_name;
	fuse_ino_t	rde_ino;
} root_direntry_t;

static root_direntry_t root_entry[] = {
	{".", FUSE_ROOT_ID},
	{"..", FUSE_ROOT_ID},
	{"mos", ROOT_DIR_MOS_ID},
	{"config", ROOT_DIR_CONFIG_ID},
	{NULL, 0}
};

static root_direntry_t config_entry[] = {
	{".", ROOT_DIR_CONFIG_ID},
	{"..", FUSE_ROOT_ID},
	{"comment", 500},
	{"root.vdev", 501},
	{"features_for_read ", 502},
	{NULL, 0}
};

#define	ROOTDIR_ENTRIES		4
#define	CONFIGDIR_ENTRIES	5
/*
 * General-purpose 64-bit bitfield encodings.
 */
#define	BF64_DECODE(x, low, len) \
	P2PHASE((x) >> (low), 1ULL << (len))

#define	BF64_ENCODE(x, low, len) \
	(P2PHASE((x), 1ULL << (len)) << (low))

#define	BF64_GET(x, low, len) \
	BF64_DECODE(x, low, len)

#define	BF64_SET(x, low, len, val) do { \
	ASSERT3U(val, <, 1ULL << (len)); \
	ASSERT3U(low + len, <=, 64); \
	((x) ^= BF64_ENCODE(((x) >> (low)) ^ (val), (low), (len))); \
} while (0)

/*
 * The bit layout of a ztour inode number is as follows:
 *
 *	64	56	48	40	32	24	16	8	0
 *	+-------+-------+-------+-------+-------+-------+-------+-------+
 * INO:	|         OBJSET        |               OBJECT               |FV|
 *	+-------+-------+-------+-------+-------+-------+-------+-------+
 *
 */

#define	INO_OBJECT_BITS		31
#define	INO_OBJSET_BITS		31	

#define	INO_OBJECT_START	2
#define	INO_OBJSET_START	(INO_OBJECT_START + INO_OBJECT_BITS)

/*
 * Macros to get/set inode number fields
 */
#define	INO_GET_OBJECT(ino)	\
	BF64_GET((ino), INO_OBJECT_START, INO_OBJECT_BITS)

#define	INO_SET_OBJECT(ino, x)	\
	BF64_SET((ino), INO_OBJECT_START, INO_OBJECT_BITS, x)

#define	INO_GET_OBJSET(ino)	\
	BF64_GET((ino), INO_OBJSET_START, INO_OBJSET_BITS)

#define	INO_SET_OS(ino, x)	\
	BF64_SET((ino), INO_OBJSET_START, INO_OBJSET_BITS, x)

#define	INO_IS_VIRTUAL(ino)	\
	BF64_GET((ino), 0, 1)

#define	INO_SET_VIRTUAL(ino, x)	\
	BF64_SET((ino), 0, 1, x)

#define	INO_INIT(ino, obj, os) do { \
	(ino) = 0; \
	INO_SET_OBJECT((ino), (obj)); \
	INO_SET_OS((ino), (os)); \
	INO_SET_VIRTUAL((ino), 0); \
} while (0)

#define	VIRTINIT(ino, obj, os, flag) do { \
	(ino) = 0; \
	INO_SET_OBJECT((ino), (obj)); \
	INO_SET_OS((ino), (os)); \
	INO_SET_VIRTUAL((ino), 1); \
} while (0)


struct objset_info {
	int		osi_object_count;
	void		*osi_objset_hndl;
	uint64_t	osi_objset_id;
	int		osi_entries;
	const char	*osi_zaptype;
	ztour_dirent_t	osi_dirent[32];
};


extern void dump_zap(void *os, uint64_t object, void *data, size_t size);


#define	MOS_NAME	"$MOS"

/*
 * Initialize ztour filesystem
 */
static void
ztour_init(void *userdata, struct fuse_conn_info *conn)
{
	const char *poolname = userdata;

	ztu_init(poolname);
}

/*
 * Clean up ztour filesystem
 */
static void
ztour_destroy(void *userdata)
{
	ztu_fini();
}

/*
 * Low level filesystem operations
 *
 * Each method receives a request handle (fuse_req_t). This request
 * must be passed to exactly one of the appropriate reply functions
 * and it is no longer valid after one of the reply functions is called.
 *
 * Other pointer arguments (name, fuse_file_info, etc) are not valid
 * after the call has returned, so if they are needed later, their
 * contents have to be copied.
 *
 * The filesystem sometimes needs to handle a return value of -ENOENT
 * from the reply function, which means, that the request was
 * interrupted, and the reply discarded.  For example if
 * fuse_reply_open() return -ENOENT means, that the release method for
 * this file will not be called.
 */

typedef struct match_info {
	const char	*mi_name;
	const char	*mi_zaptype;
	void		*mi_handle;
	uint64_t	mi_objset;
	uint64_t	mi_parent;	/* parent object */
	uint64_t	mi_object;	/* objset object */
	struct stat	mi_stat;
} match_info_t;

/*
 * match zap entry by name or ino to get attributes
 */
static void
zap_match_cb(const char *name, uint64_t value, uint_t index, void *arg)
{
	match_info_t *match = arg;

	if ((match->mi_object != 0 && match->mi_object == value) ||
	   (match->mi_name != NULL && strcmp(name, match->mi_name) == 0)) {
		if (strcmp(match->mi_zaptype, "zap") == 0) {
			value = 10000 + index;
			match->mi_stat.st_mode = S_IFREG | 0444;
			match->mi_stat.st_nlink = 1;
		} else if (ztu_object_isdir(match->mi_handle, value)) {
			match->mi_stat.st_mode = S_IFDIR | 0555;
			match->mi_stat.st_nlink = 2;
		} else {
			match->mi_stat.st_mode = S_IFREG | 0444;
			match->mi_stat.st_nlink = 1;
		}

		INO_INIT(match->mi_stat.st_ino, value, match->mi_object);
		match->mi_stat.st_size = 2;
		match->mi_stat.st_atim.tv_sec = 1440402672ULL;
		match->mi_stat.st_ctim.tv_sec = 1440402672ULL;
		match->mi_stat.st_mtim.tv_sec = 1440402672ULL;

		(void) printf("\t%s[%d]: %s, %d\n", __func__, index, name,
		    (int)value);
	}
}

/*
 * Look up a directory entry by name and get its attributes.
 *
 * Valid replies:
 *   fuse_reply_entry
 *   fuse_reply_err
 *
 * @param req request handle
 * @param parent inode number of the parent directory
 * @param name the name to look up
 */
static void
ztour_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
        struct fuse_entry_param entry = { 0 };

	if (strcmp(name, ".directory") == 0) {
		(void) fprintf(stderr, "\tENOENT\n");
		fuse_reply_err(req, ENOENT);
		return;
	}

	(void) fprintf(stderr, "%s(os %llu, obj %llu, '%s')\n", __func__,
	    INO_GET_OBJSET(parent), INO_GET_OBJECT(parent), name);

	if (parent == FUSE_ROOT_ID) {
		for (int i = 0; i < ROOTDIR_ENTRIES; i++) {
			if (strcmp(name, root_entry[i].rde_name) == 0) {
				entry.ino = root_entry[i].rde_ino;
				entry.attr_timeout = 1.0;
				entry.entry_timeout = 1.0;
				entry.attr.st_ino = entry.ino;
				entry.attr.st_mode = S_IFDIR | 0555;
				entry.attr.st_size = 3;
				entry.attr.st_nlink = 3;
				entry.attr.st_atim.tv_sec = 1440402672ULL;
				entry.attr.st_ctim.tv_sec = 1440402672ULL;
				entry.attr.st_mtim.tv_sec = 1440402672ULL;
				fuse_reply_entry(req, &entry);
				return;
			}
		}
		(void) fprintf(stderr, "\tENOENT (root)\n");
		fuse_reply_err(req, ENOENT);
		return;
	}

	if (parent == ROOT_DIR_CONFIG_ID) {
		for (int i = 0; i < CONFIGDIR_ENTRIES; i++) {
			if (strcmp(name, config_entry[i].rde_name) == 0) {
				entry.ino = config_entry[i].rde_ino;
				entry.attr_timeout = 1.0;
				entry.entry_timeout = 1.0;
				entry.attr.st_ino = entry.ino;
				entry.attr.st_mode = S_IFDIR | 0555;
				entry.attr.st_size = 4096;
				entry.attr.st_nlink = CONFIGDIR_ENTRIES + 2;
				entry.attr.st_atim.tv_sec = 1440402672ULL;
				entry.attr.st_ctim.tv_sec = 1440402672ULL;
				entry.attr.st_mtim.tv_sec = 1440402672ULL;
				fuse_reply_entry(req, &entry);
				return;
			}
		}
		(void) fprintf(stderr, "\tENOENT (root)\n");
		fuse_reply_err(req, ENOENT);
		return;
	}

	const char *dsname = NULL;
	char dataset[256];
#if 0
	if (parent != FUSE_ROOT_ID &&
	    parent != ROOT_DIR_MOS_ID &&
	    INO_GET_OBJSET(parent) != 0) {
		fuse_reply_err(req, ENOENT);
		return;
	}
#endif
	if (!INO_IS_VIRTUAL(parent) && INO_GET_OBJSET(parent) == 0) {
		dsname = MOS_NAME;
	} else /* dataset */ {
		uint64_t object = INO_GET_OBJECT(parent);

		if (object > 0 &&
		    ztu_objset_name(object, dataset, sizeof (dataset)) == 0) {
			(void) fprintf(stdout, "zt_lookup: datset obj %lu path "
			    "\"%s\"\n", object, dataset);
			dsname = dataset;
		}
	}

	if (dsname != NULL) {
		void *handle;
		int count;

		if (ztu_open_objset(dsname, &handle, &count) != 0) {
			fuse_reply_err(req, ENOENT);
			return;
		}
		match_info_t match = {0};

		match.mi_name = name;
		match.mi_zaptype = ztu_object_type(handle,
		    INO_GET_OBJECT(parent)); 
		match.mi_handle = handle;
		(void) printf("\t zap type '%s'\n", match.mi_zaptype);

		ztu_for_each_zap(handle, INO_GET_OBJECT(parent), zap_match_cb,
		    &match);

		if (match.mi_stat.st_mode != 0) {
			entry.ino = match.mi_stat.st_ino;
			entry.attr_timeout = 1.0;
			entry.entry_timeout = 1.0;
			entry.attr.st_ino = entry.ino;
			entry.attr.st_mode = match.mi_stat.st_mode;
			entry.attr.st_size = match.mi_stat.st_size;
			entry.attr.st_nlink = match.mi_stat.st_nlink;
			entry.attr.st_atim.tv_sec =
			    match.mi_stat.st_atim.tv_sec;
			entry.attr.st_ctim.tv_sec =
			    match.mi_stat.st_ctim.tv_sec;
			entry.attr.st_mtim.tv_sec =
			    match.mi_stat.st_mtim.tv_sec;

			fuse_reply_entry(req, &entry);
			return;
		} else {
			(void) fprintf(stderr, "\t no match!\n");
		}

	}

	(void) fprintf(stderr, "\tENOENT\n");
	fuse_reply_err(req, ENOENT);
}

/*
 * Forget about an inode
 *
 * This function is called when the kernel removes an inode
 * from its internal caches.
 *
 * The inode's lookup count increases by one for every call to
 * fuse_reply_entry and fuse_reply_create. The nlookup parameter
 * indicates by how much the lookup count should be decreased.
 *
 * Inodes with a non-zero lookup count may receive request from
 * the kernel even after calls to unlink, rmdir or (when
 * overwriting an existing file) rename. Filesystems must handle
 * such requests properly and it is recommended to defer removal
 * of the inode until the lookup count reaches zero. Calls to
 * unlink, remdir or rename will be followed closely by forget
 * unless the file or directory is open, in which case the
 * kernel issues forget only after the release or releasedir
 * calls.
 *
 * Note that if a file system will be exported over NFS the
 * inodes lifetime must extend even beyond forget. See the
 * generation field in struct fuse_entry_param above.
 *
 * On unmount the lookup count for all inodes implicitly drops
 * to zero. It is not guaranteed that the file system will
 * receive corresponding forget messages for the affected
 * inodes.
 *
 * Valid replies:
 *   fuse_reply_none
 *
 * @param req request handle
 * @param ino the inode number
 * @param nlookup the number of lookups to forget
 */
static void
ztour_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
	(void) fprintf(stderr, "%s(%lu)\n", __func__, ino);
	fuse_reply_none(req);
}

/*
 * Get object attributes
 *
 * Valid replies:
 *   fuse_reply_attr
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param fi for future use, currently always NULL
 */
static void
ztour_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct stat stbuf = { 0 };

	(void) fprintf(stderr, "%s(%lu)\n", __func__, ino);

	if (ino == FUSE_ROOT_ID) {
		stbuf.st_ino = ino;
		stbuf.st_mode = S_IFDIR | 0755;
		stbuf.st_nlink = 3;
		stbuf.st_size = 3;
		stbuf.st_atim.tv_sec = 1440402672ULL;
		stbuf.st_ctim.tv_sec = 1440402672ULL;
		stbuf.st_mtim.tv_sec = 1440402672ULL;
		fuse_reply_attr(req, &stbuf, 1.0);
	} else if (ino == ROOT_DIR_MOS_ID || ino == ROOT_DIR_CONFIG_ID) {
		stbuf.st_ino = ino;
		stbuf.st_mode = S_IFDIR | 0755;
		stbuf.st_nlink = 5;
		stbuf.st_size = 5;
		stbuf.st_atim.tv_sec = 1440402672ULL;
		stbuf.st_ctim.tv_sec = 1440402672ULL;
		stbuf.st_mtim.tv_sec = 1440402672ULL;
		fuse_reply_attr(req, &stbuf, 1.0);
	} else if (INO_GET_OBJSET(ino) == 0) {
		stbuf.st_ino = ino;
		stbuf.st_mode = S_IFDIR | 0755;
		stbuf.st_nlink = 2;
		stbuf.st_size = 2;
		stbuf.st_atim.tv_sec = 1440402672ULL;
		stbuf.st_ctim.tv_sec = 1440402672ULL;
		stbuf.st_mtim.tv_sec = 1440402672ULL;
		fuse_reply_attr(req, &stbuf, 1.0);
	} else {
		fuse_reply_err(req, ENOENT);
	}
}

/*
 * Read symbolic link
 *
 * Valid replies:
 *   fuse_reply_readlink
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 */
static void
ztour_readlink(fuse_req_t req, fuse_ino_t ino)
{
	(void) fprintf(stderr, "%s(%lu))\n", __func__, ino);
	fuse_reply_err(req, EINVAL);
}

/*
 * Open a file
 *
 * Open flags (with the exception of O_CREAT, O_EXCL, O_NOCTTY and
 * O_TRUNC) are available in fi->flags.
 *
 * Filesystem may store an arbitrary file handle (pointer, index,
 * etc) in fi->fh, and use this in other all other file operations
 * (read, write, flush, release, fsync).
 *
 * Filesystem may also implement stateless file I/O and not store
 * anything in fi->fh.
 *
 * There are also some flags (direct_io, keep_cache) which the
 * filesystem may set in fi, to change the way the file is opened.
 * See fuse_file_info structure in <fuse_common.h> for more details.
 *
 * Valid replies:
 *   fuse_reply_open
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param fi file information
 */
static void
ztour_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	(void) fprintf(stderr, "%s(%lu)\n", __func__, ino);
	fuse_reply_err(req, EINVAL);
}

/*
 * Read data
 *
 * Read should send exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the file
 * has been opened in 'direct_io' mode, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * fi->fh will contain the value set by the open method, or will
 * be undefined if the open method didn't set any value.
 *
 * Valid replies:
 *   fuse_reply_buf
 *   fuse_reply_iov
 *   fuse_reply_data
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param size number of bytes to read
 * @param off offset to read from
 * @param fi file information
 */
static void
ztour_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    struct fuse_file_info *fi)
{
	(void) fprintf(stderr, "%s(%lu)\n", __func__, ino);
	fuse_reply_err(req, EINVAL);
}

/*
 * Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open call there will be exactly one release call.
 *
 * The filesystem may reply with an error, but error values are
 * not returned to close() or munmap() which triggered the
 * release.
 *
 * fi->fh will contain the value set by the open method, or will
 * be undefined if the open method didn't set any value.
 * fi->flags will contain the same flags as for open.
 *
 * Valid replies:
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param fi file information
 */
static void
ztour_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	(void) fprintf(stderr, "%s(%lu)\n", __func__, ino);
	fuse_reply_err(req, EINVAL);
}



static void
zap_visit_obj_cb(const char *name, uint64_t value, uint_t index, void *arg)
{
	struct objset_info *osi = arg;
	int i= osi->osi_entries;

	if (strcmp(name, "creation_version") == 0 ||
	    strcmp(name, "deflate") == 0)
		return;

	if (strcmp(osi->osi_zaptype, "zap") == 0) {
		value = 10000 + index;
		osi->osi_dirent[i].ztd_type = S_IFREG;
	} else if (ztu_object_isdir(osi->osi_objset_hndl, value)) {
		osi->osi_dirent[i].ztd_type = S_IFDIR;
	} else {
		osi->osi_dirent[i].ztd_type = S_IFREG;
	}
	INO_INIT(osi->osi_dirent[i].ztd_ino, value, 0);
	osi->osi_dirent[i].ztd_position = i + 2;
	(void) strncpy(osi->osi_dirent[i].ztd_name, name,
		sizeof (osi->osi_dirent[i].ztd_name));
	osi->osi_entries++;

	(void) printf("\t%s [%d] %s, %d\n", __func__, index, name, (int)value);
}


/*
 * Open a directory
 *
 * Filesystem may store an arbitrary file handle (pointer, index,
 * etc) in fi->fh, and use this in other all other directory
 * stream operations (readdir, releasedir, fsyncdir).
 *
 * Filesystem may also implement stateless directory I/O and not
 * store anything in fi->fh, though that makes it impossible to
 * implement standard conforming directory stream operations in
 * case the contents of the directory can change between opendir
 * and releasedir.
 *
 * Valid replies:
 *   fuse_reply_open
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param fi file information
 */
static void
ztour_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	const char *dsname = NULL;
	char dataset[256];

	fi->fh = 0ULL;

	(void) fprintf(stderr, "%s(%lu)\n", __func__, ino);

	if (ino != FUSE_ROOT_ID &&
	    ino != ROOT_DIR_MOS_ID &&
	    INO_GET_OBJSET(ino) != 0) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	if (!INO_IS_VIRTUAL(ino) && INO_GET_OBJSET(ino) == 0) {
		dsname = MOS_NAME;
	} else /* dataset */ {
		uint64_t object = INO_GET_OBJECT(ino);

		if (object > 0 &&
		    ztu_objset_name(object, dataset, sizeof (dataset)) == 0) {
			(void) fprintf(stdout, "zt_lookup: datset obj %lu path "
			    "\"%s\"\n", object, dataset);
			dsname = dataset;
		}
	}

	if (dsname != NULL) {
		struct objset_info *osi;
		void *handle;
		int count;

		if (ztu_open_objset(dsname, &handle, &count) != 0) {
			fuse_reply_err(req, ENOENT);
			return;
		}
		osi = malloc(sizeof (*osi));
		osi->osi_object_count = count + 1;	/* account for master dnode */
		osi->osi_objset_hndl = handle;
		fi->fh = (uint64_t)osi;
		osi->osi_zaptype = ztu_object_type(handle, INO_GET_OBJECT(ino)); 
		(void) printf("\t zap type '%s'\n", osi->osi_zaptype);
		(void) fprintf(stderr, "zt_opendir: '%s' with %d objs\n",
		    dsname, count);

		osi->osi_entries = 0;
		ztu_for_each_zap(handle, INO_GET_OBJECT(ino),
		    zap_visit_obj_cb, osi);
	}

	(void) fprintf(stderr, "zt_opendir: obj %d in os %d, flg 0x%x\n",
		(int)INO_GET_OBJECT(ino), (int)INO_GET_OBJSET(ino),
		(int)INO_IS_VIRTUAL(ino));

	fuse_reply_open(req, fi);
}

/*
 * Release an open directory
 *
 * For every opendir call there will be exactly one releasedir
 * call.
 *
 * fi->fh will contain the value set by the opendir method, or
 * will be undefined if the opendir method didn't set any value.
 *
 * Valid replies:
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param fi file information
 */
static void
ztour_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	(void) fprintf(stderr, "%s(%lu), %d bytes\n", __func__, ino,
	    (int)sizeof (ino));
	fuse_reply_err(req, EINVAL);
	
	if (fi->fh != 0) {
		struct objset_info *osi = (struct objset_info *)fi->fh;

		fi->fh = 0;
		(void) ztu_close_objset(osi->osi_objset_hndl);
		free(osi);
	}
}

static void
ztour_readdir_root(fuse_req_t req, size_t size, off_t off,
    struct fuse_file_info *fi)
{
	char *buf, *cur;
	size_t resid = size;
	int i;

	(void) fprintf(stderr, "%s(%d bytes, at %d)\n", __func__, (int)size,
	    (int)off);

	if (off > ROOTDIR_ENTRIES-1) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}

	cur = buf = malloc(size);

	for (i = off; i < ROOTDIR_ENTRIES; i++) {
		struct stat stbuf = { 0 };
		size_t used;

		stbuf.st_ino = root_entry[i].rde_ino;
		stbuf.st_mode = S_IFDIR;

		used = fuse_add_direntry(req, cur, resid,
		    root_entry[i].rde_name, &stbuf, i+1);
		if (used > resid)
			break;

		resid -= used;
		cur += used;
		if (resid <= 0)
			break;
	}

	fuse_reply_buf(req, buf, size - resid);
}

static void
ztour_readdir_config(fuse_req_t req, size_t size, off_t off,
    struct fuse_file_info *fi)
{
	char *buf, *cur;
	size_t resid = size;
	int i;

	(void) fprintf(stderr, "%s(%d bytes, at %d)\n", __func__, (int)size,
	    (int)off);

	if (off > CONFIGDIR_ENTRIES-1) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}

	cur = buf = malloc(size);

	for (i = off; i < CONFIGDIR_ENTRIES; i++) {
		struct stat stbuf = { 0 };
		size_t used;

		stbuf.st_ino = config_entry[i].rde_ino;
		stbuf.st_mode = S_IFDIR;

		used = fuse_add_direntry(req, cur, resid,
		    config_entry[i].rde_name, &stbuf, i+1);
		if (used > resid)
			break;

		resid -= used;
		cur += used;
		if (resid <= 0)
			break;
	}

	fuse_reply_buf(req, buf, size - resid);
}

/*
 * Read directory
 *
 * Send a buffer filled using fuse_add_direntry(), with size not
 * exceeding the requested size.  Send an empty buffer on end of
 * stream.
 *
 * fi->fh will contain the value set by the opendir method, or
 * will be undefined if the opendir method didn't set any value.
 *
 * Valid replies:
 *   fuse_reply_buf
 *   fuse_reply_data
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param size maximum number of bytes to send
 * @param off offset to continue reading the directory stream
 * @param fi file information
 *
 * offset
 *	  0 = "."
 *	  1 = ".."
 *	  2 = 1st entry
 *	256 = 2nd entry
 *	512 = 3rd entry
 *	...
 *	off >> 9
 */
static void
ztour_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
    struct fuse_file_info *fi)
{
	struct objset_info *osi = (struct objset_info *)fi->fh;
	char *buf, *cur;
	size_t resid = size;
	size_t used;
	struct stat stbuf = { 0 };

	(void) fprintf(stderr, "zt_readdir(ino %lu, %d bytes, at %d)\n", ino,
	    (int)size, (int)off);

	if (ino == FUSE_ROOT_ID) {
                ztour_readdir_root(req, size, off, fi);
                return;
        }

	if (ino == ROOT_DIR_CONFIG_ID) {
                ztour_readdir_config(req, size, off, fi);
                return;
        }

	if (INO_IS_VIRTUAL(ino) ||  INO_GET_OBJSET(ino) != 0) {
		fuse_reply_err(req, ENOENT);
                return;
	}

	if (off > 0) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}

	cur = buf = malloc(resid);

	stbuf.st_ino = ino;	/* our parent */
	used = fuse_add_direntry(req, cur, resid, ".", &stbuf, 32);
	resid -= used;
	cur += used;

	stbuf.st_ino = FUSE_ROOT_ID;	/* filled in by FUSE */
	used = fuse_add_direntry(req, cur, resid, "..", &stbuf, 64);
	resid -= used;
	cur += used;

	if (osi == NULL || osi->osi_entries == 0) {
		fuse_reply_buf(req, buf, size - resid);
		free(buf);
		return;
	}

	for (int i = 0; i < osi->osi_entries; i++) {
		stbuf.st_ino = osi->osi_dirent[i].ztd_ino;
		stbuf.st_mode = osi->osi_dirent[i].ztd_type;
		used = fuse_add_direntry(req, cur, resid,
		    osi->osi_dirent[i].ztd_name, &stbuf, (i+1) << 9);
		if (used > resid)
			break;

		resid -= used;
		cur += used;
		if (resid <= 0)
			break;
	}

	fuse_reply_buf(req, buf, size - resid);
	free(buf);

//	(void) fprintf(stderr, "%d entries packed, reply size %d\n",
//	    entries, (int)(size - resid));
}


/*
 * Get file system statistics
 *
 * Valid replies:
 *   fuse_reply_statfs
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number, zero means "undefined"
 */
static void
ztour_statfs(fuse_req_t req, fuse_ino_t ino)
{
	(void) fprintf(stderr, "%s(%lu)\n", __func__, ino);
	fuse_reply_err(req, EINVAL);
}

/*
 * Get an extended attribute
 *
 * If size is zero, the size of the value should be sent with
 * fuse_reply_xattr.
 *
 * If the size is non-zero, and the value fits in the buffer, the
 * value should be sent with fuse_reply_buf.
 *
 * If the size is too small for the value, the ERANGE error should
 * be sent.
 *
 * Valid replies:
 *   fuse_reply_buf
 *   fuse_reply_data
 *   fuse_reply_xattr
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param name of the extended attribute
 * @param size maximum size of the value to send
 */
static void
ztour_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
	size_t len = 0;
	char *buf = NULL;

	if (strstr(name, "user.") == name ||
	    strstr(name, "system.") == name ||
	    strstr(name, "security.") == name) {
		fuse_reply_err(req, ENODATA);
		return;
	}

	(void) fprintf(stderr, "%s(ino %lu, '%s', %d)\n", __func__, ino, name,
	    (int)size);

	if (ino != FUSE_ROOT_ID && ino != ROOT_DIR_MOS_ID) {
		fuse_reply_err(req, ENODATA);
		return;
	}

	if (strcmp(name, "user.ub.magic") == 0)
		len = asprintf(&buf, "0000000000bab10c");
	else if (strcmp(name, "user.ub.version") == 0)
		len = asprintf(&buf, "5000");
	else if (strcmp(name, "user.ub.txg") == 0)
		len = asprintf(&buf, "36376");
	else if (strcmp(name, "user.ub.guid_sum") == 0)
		len = asprintf(&buf, "7162048158917478386");
	else if (strcmp(name, "user.ub.checksum") == 0)
		len = asprintf(&buf,
		    "aed81937a:4396d25b99f:d840d019e43b:1dc53a66c3909e");
	else if (strcmp(name, "user.ub.compression") == 0)
		len = asprintf(&buf, "lz4");
	else if (strcmp(name, "user.ub.birth") == 0)
		len = asprintf(&buf, "36376");

	if (buf != NULL && len > 0) {
		if (size == 0)
			fuse_reply_xattr(req, len);
		else
			fuse_reply_buf(req, buf, len);
		free(buf);
	} else {
		fuse_reply_err(req, ENODATA);
	}
}

/*
 * List extended attribute names
 *
 * If size is zero, the total size of the attribute list should be
 * sent with fuse_reply_xattr.
 *
 * If the size is non-zero, and the null character separated
 * attribute list fits in the buffer, the list should be sent with
 * fuse_reply_buf.
 *
 * If the size is too small for the list, the ERANGE error should
 * be sent.
 *
 * Valid replies:
 *   fuse_reply_buf
 *   fuse_reply_data
 *   fuse_reply_xattr
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param size maximum size of the list to send
 */
static void
ztour_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
	size_t len = 0;
	char *buf = NULL;

	(void) fprintf(stderr, "%s(ino %lu, %d bytes)\n", __func__, ino,
	    (int)size);

	if (ino == FUSE_ROOT_ID && ino == ROOT_DIR_MOS_ID) {
		size_t resid = 256;
		char *slot;

		slot = buf = malloc(resid);

		len = snprintf(slot, resid, "%s", "user.ub.magic");
		slot += len + 1;
		resid -= len + 1;

		len = snprintf(slot, resid, "%s", "user.ub.version");
		slot += len + 1;
		resid -= len + 1;

		len = snprintf(slot, resid, "%s", "user.ub.txg");
		slot += len + 1;
		resid -= len + 1;

		len = snprintf(slot, resid, "%s", "user.ub.guid_sum");
		slot += len + 1;
		resid -= len + 1;

		len = snprintf(slot, resid, "%s", "user.ub.checksum");
		slot += len + 1;
		resid -= len + 1;

		len = snprintf(slot, resid, "%s", "user.ub.compression");
		slot += len + 1;
		resid -= len + 1;

		len = snprintf(slot, resid, "%s", "user.ub.birth");
		slot += len + 1;
		resid -= len + 1;

		len = 256 - resid;
	}

	if (size == 0)
		fuse_reply_xattr(req, len);
	else
		fuse_reply_buf(req, buf, len);

	if (buf)
		free(buf);
}

/*
 * Check file access permissions
 *
 * This will be called for the access() system call.  If the
 * 'default_permissions' mount option is given, this method is not
 * called.
 *
 * Valid replies:
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param mask requested access mode
 */
static void
ztour_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
	(void) fprintf(stderr, "%s(%lu)\n", __func__, ino);
	fuse_reply_err(req, 0);
}

/*
 * Ioctl
 *
 * Note: For unrestricted ioctls (not allowed for FUSE
 * servers), data in and out areas can be discovered by giving
 * iovs and setting FUSE_IOCTL_RETRY in @flags.  For
 * restricted ioctls, kernel prepares in/out data area
 * according to the information encoded in cmd.
 *
 * Introduced in version 2.8
 *
 * Valid replies:
 *   fuse_reply_ioctl_retry
 *   fuse_reply_ioctl
 *   fuse_reply_ioctl_iov
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param cmd ioctl command
 * @param arg ioctl argument
 * @param fi file information
 * @param flags for FUSE_IOCTL_* flags
 * @param in_buf data fetched from the caller
 * @param in_bufsz number of fetched bytes
 * @param out_bufsz maximum size of output data
 */
static void
ztour_ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void *arg,
    struct fuse_file_info *fi, unsigned flags, const void *in_buf,
    size_t in_bufsz, size_t out_bufsz)
{
	(void) fprintf(stderr, "%s(%lu, %d)\n", __func__, ino, cmd);
	fuse_reply_err(req, EINVAL);
}

/*
 * Callback function for the retrieve request
 *
 * Introduced in version 2.9
 *
 * Valid replies:
 *	fuse_reply_none
 *
 * @param req request handle
 * @param cookie user data supplied to fuse_lowlevel_notify_retrieve()
 * @param ino the inode number supplied to fuse_lowlevel_notify_retrieve()
 * @param offset the offset supplied to fuse_lowlevel_notify_retrieve()
 * @param bufv the buffer containing the returned data
 */
static void
ztour_retrieve_reply(fuse_req_t req, void *cookie, fuse_ino_t ino, off_t offset,
    struct fuse_bufvec *bufv)
{
	(void) fprintf(stderr, "%s(%lu)\n", __func__, ino);
	fuse_reply_none(req);
}

/*
 * Forget about multiple inodes
 *
 * See description of the forget function for more
 * information.
 *
 * Introduced in version 2.9
 *
 * Valid replies:
 *   fuse_reply_none
 *
 * @param req request handle
 */
static void
ztour_forget_multi(fuse_req_t req, size_t count,
    struct fuse_forget_data *forgets)
{
	(void) fprintf(stderr, "%s()\n", __func__);
	fuse_reply_none(req);
}

static struct fuse_lowlevel_ops ztour_ops = {
	.init = ztour_init,
	.destroy = ztour_destroy,
	.lookup = ztour_lookup,
	.forget = ztour_forget,
	.getattr = ztour_getattr,
	.readlink = ztour_readlink,
	.open = ztour_open,
	.read = ztour_read,
	.release = ztour_release,
	.opendir = ztour_opendir,
	.readdir = ztour_readdir,
	.releasedir = ztour_releasedir,
	.statfs = ztour_statfs,
	.getxattr = ztour_getxattr,
	.listxattr = ztour_listxattr,
	.access = ztour_access,
	.ioctl = ztour_ioctl,
	.retrieve_reply = ztour_retrieve_reply,
	.forget_multi = ztour_forget_multi
};

/*
 * mount options
 */
char* mnt_argv[] = { "-o", "ro,noatime,nodiratime,noexec", NULL };

/*
 * fuse options
 */
char* fuse_argv[] = { "-ofsname=ztour", NULL };

static void
usage(void)
{
	(void) fprintf(stderr, "Usage: <pool> <mountpoint>\n");
	exit (1);
}

#define FUSE_ARGS_INIT2(argv) \
	FUSE_ARGS_INIT(sizeof(argv)/sizeof(argv[0]) - 1, argv)


int
main(int argc, char *argv[])
{
	struct fuse_args mnt_args = FUSE_ARGS_INIT2(mnt_argv);
	struct fuse_args opt_args = FUSE_ARGS_INIT2(fuse_argv);
	struct fuse_chan *channel;
	struct fuse_session *session;
	char *mountpoint, *poolname;
	int err = -1;

	if (argc < 3) {
		(void) fprintf(stderr, "missing required parameters\n");
		usage();
	}

	poolname = argv[1];
	mountpoint = argv[2];

	if ((channel = fuse_mount(mountpoint, &mnt_args)) == NULL) {
		(void) fprintf(stdout, "ERROR: fuse_mount failed\n");
		exit (1);
	}

	/*
	 * Instantiate a new file system
	 * Will init the ZDB backend
	 */
	if ((session = fuse_lowlevel_new(&opt_args, &ztour_ops,
	    sizeof(ztour_ops), poolname)) == NULL) {
		(void) fprintf(stdout, "ERROR: fuse_lowlevel_new failed\n");
		fuse_unmount(mountpoint, channel);
		exit (1);
	}

	if (fuse_set_signal_handlers(session) == -1) {
		(void) fprintf(stdout, "ERROR: fuse_set_signal_handlers "
		    "failed\n");
		fuse_session_destroy(session);
		fuse_unmount(mountpoint, channel);
		exit (1);
	}

	fuse_session_add_chan(session, channel);

	/*
	 * Block until ctrl+c or fusermount -u
	 */
	err = fuse_session_loop(session);

	fuse_remove_signal_handlers(session);
	fuse_session_remove_chan(channel);
	fuse_session_destroy(session);
	fuse_unmount(mountpoint, channel);

	return err ? 1 : 0;
}
