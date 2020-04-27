/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

#ifndef ZFS_VFSOPS_OS_H
#define	ZFS_VFSOPS_OS_H

#include <sys/dataset_kstats.h>
#include <sys/isa_defs.h>
#include <sys/types32.h>
#include <sys/list.h>
// #include <sys/vfs.h>
#include <sys/zil.h>
#include <sys/sa.h>
#include <sys/rrwlock.h>
#include <sys/dsl_dataset.h>
#include <sys/zfs_ioctl.h>
#include <sys/objlist.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct zfsvfs zfsvfs_t;

struct zfsvfs {
//	vfs_t		*z_vfs;		/* generic fs struct */
//	struct super_block *z_sb;	/* generic super_block */
	struct zfsvfs	*z_parent;	/* parent fs */
	objset_t	*z_os;		/* objset reference */
	uint64_t	z_flags;	/* super_block flags */
	uint64_t	z_root;		/* id of root znode */
	uint64_t	z_unlinkedobj;	/* id of unlinked zapobj */
	uint64_t	z_max_blksz;	/* maximum block size for files */
	uint64_t	z_fuid_obj;	/* fuid table object number */
	uint64_t	z_fuid_size;	/* fuid table size */
	avl_tree_t	z_fuid_idx;	/* fuid tree keyed by index */
	avl_tree_t	z_fuid_domain;	/* fuid tree keyed by domain */
	krwlock_t	z_fuid_lock;	/* fuid lock */
	boolean_t	z_fuid_loaded;	/* fuid tables are loaded */
	boolean_t	z_fuid_dirty;   /* need to sync fuid table ? */
	struct zfs_fuid_info	*z_fuid_replay; /* fuid info for replay */
	zilog_t		*z_log;		/* intent log pointer */
	uint_t		z_acl_inherit;	/* acl inheritance behavior */
	uint_t		z_acl_type;	/* type of ACL usable on this FS */
	zfs_case_t	z_case;		/* case-sense */
	boolean_t	z_utf8;		/* utf8-only */
	int		z_norm;		/* normalization flags */
	boolean_t	z_relatime;	/* enable relatime mount option */
	boolean_t	z_unmounted;	/* unmounted */
	rrmlock_t	z_teardown_lock;
	krwlock_t	z_teardown_inactive_lock;
	list_t		z_all_znodes;	/* all znodes in the fs */
	uint64_t	z_nr_znodes;	/* number of znodes in the fs */
	unsigned long	z_rollback_time; /* last online rollback time */
	unsigned long	z_snap_defer_time; /* last snapshot unmount deferral */
	kmutex_t	z_znodes_lock;	/* lock for z_all_znodes */
	arc_prune_t	*z_arc_prune;	/* called by ARC to prune caches */
	struct inode	*z_ctldir;	/* .zfs directory inode */
	boolean_t	z_show_ctldir;	/* expose .zfs in the root dir */
	boolean_t	z_issnap;	/* true if this is a snapshot */
	boolean_t	z_vscan;	/* virus scan on/off */
	boolean_t	z_use_fuids;	/* version allows fuids */
	boolean_t	z_replay;	/* set during ZIL replay */
	boolean_t	z_use_sa;	/* version allow system attributes */
	boolean_t	z_xattr_sa;	/* allow xattrs to be stores as SA */
	boolean_t	z_draining;	/* is true when drain is active */
	boolean_t	z_drain_cancel; /* signal the unlinked drain to stop */
	uint64_t	z_version;	/* ZPL version */
	uint64_t	z_shares_dir;	/* hidden shares dir */
	dataset_kstats_t	z_kstat;	/* fs kstats */
	kmutex_t	z_lock;
	uint64_t	z_userquota_obj;
	uint64_t	z_groupquota_obj;
	uint64_t	z_userobjquota_obj;
	uint64_t	z_groupobjquota_obj;
	uint64_t	z_projectquota_obj;
	uint64_t	z_projectobjquota_obj;
	uint64_t	z_replay_eof;	/* New end of file - replay only */
	sa_attr_type_t	*z_attr_table;	/* SA attr mapping->id */
	uint64_t	z_hold_size;	/* znode hold array size */
	avl_tree_t	*z_hold_trees;	/* znode hold trees */
	kmutex_t	*z_hold_locks;	/* znode hold locks */
	taskqid_t	z_drain_task;	/* task id for the unlink drain task */
};

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_FS_ZFS_VFSOPS_H */
