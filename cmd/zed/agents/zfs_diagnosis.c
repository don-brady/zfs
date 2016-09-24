/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
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

/*
 * Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
 */

#include <strings.h>
#include <libuutil.h>
#include <libzfs.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/fs/zfs.h>
#include <sys/fm/protocol.h>
#include <sys/sysevent/eventdefs.h>
#include <sys/sysevent/dev.h>
#include <sys/fm/fs/zfs.h>

#include "zfs_agents.h"
#include "fmd_serd.h"
#include "../zed_log.h"

#define	DE_TAG "\x1B[35mDE:\x1B[0m "


#define	FMD_NOSLEEP	0x0	/* do not sleep or retry if alloc fails */
#define	FMD_SLEEP	0x1	/* sleep or retry if alloc fails */

static void *fmd_zalloc(size_t size, int flags);
static void fmd_free(void *data, size_t size);

int fmd_nvl_class_match(nvlist_t *nvl, const char *pattern);
static nvlist_t * fmd_nvl_alloc(int flags);
static nvlist_t * fmd_nvl_create_fault(const char *class, uint8_t certainty,
    nvlist_t *asru, nvlist_t *fru, nvlist_t *resource);

struct fmd_event;
typedef struct fmd_event fmd_event_t;

struct fmd_hdl;
typedef struct fmd_hdl fmd_hdl_t;


static timer_t fmd_timer_install(void *arg, fmd_event_t *ep, hrtime_t delta);
static void fmd_timer_remove(timer_t id);

int fmd_nvl_fmri_present(nvlist_t *nvl);
int fmd_nvl_fmri_unusable(nvlist_t *nvl);



/*
 * Our serd engines are named 'zfs_<pool_guid>_<vdev_guid>_{checksum,io}'.  This
 * #define reserves enough space for two 64-bit hex values plus the length of
 * the longest string.
 */
#define	MAX_SERDLEN	(16 * 2 + sizeof ("zfs___checksum"))

/*
 * On-disk case structure.  This must maintain backwards compatibility with
 * previous versions of the DE.  By default, any members appended to the end
 * will be filled with zeros if they don't exist in a previous version.
 */
typedef struct zfs_case_data {
	uint64_t	zc_version;
	uint64_t	zc_ena;
	uint64_t	zc_pool_guid;
	uint64_t	zc_vdev_guid;
	int		zc_pool_state;
	char		zc_serd_checksum[MAX_SERDLEN];
	char		zc_serd_io[MAX_SERDLEN];
	int		zc_has_remove_timer;
} zfs_case_data_t;

/*
 * Time-of-day
 */
typedef struct er_timeval {
	uint64_t	ertv_sec;
	uint64_t	ertv_nsec;
} er_timeval_t;


/*
 * In-core case structure.
 */
typedef struct zfs_case {
	boolean_t	zc_present;
	uint32_t	zc_version;
	zfs_case_data_t	zc_data;
	uint32_t	zc_case_state;
	uu_list_node_t	zc_node;
	timer_t		zc_remove_timer;
	char		*zc_fru;	/* FRU not yet available on Linux */
	er_timeval_t	zc_when;
} zfs_case_t;

#define	CASE_DATA			"data"
#define	CASE_FRU			"fru"
#define	CASE_DATA_VERSION_INITIAL	1
#define	CASE_DATA_VERSION_SERD		2

#define	FMD_CASE_UNSOLVED	0	/* case is not yet solved (waiting) */
#define	FMD_CASE_SOLVED		1	/* case is solved (suspects added) */
#define	FMD_CASE_CLOSE_WAIT	2	/* case is executing fmdo_close() */
#define	FMD_CASE_CLOSED		3	/* case is closed (reconfig done) */
#define	FMD_CASE_REPAIRED	4	/* case is repaired */
#define	FMD_CASE_RESOLVED	5	/* case is resolved (can be freed) */


typedef struct zfs_de_stats {
	uint64_t	old_drops;
	uint64_t	dev_drops;
	uint64_t	vdev_drops;
	uint64_t	import_drops;
	uint64_t	resource_drops;
} zfs_de_stats_t;

zfs_de_stats_t zfs_stats;

static hrtime_t zfs_remove_timeout;

uu_list_pool_t *zfs_case_pool;
uu_list_t *zfs_cases;

libzfs_handle_t *g_zfshdl;

#define	ZFS_MAKE_RSRC(type)	\
    FM_RSRC_CLASS "." ZFS_ERROR_CLASS "." type
#define	ZFS_MAKE_EREPORT(type)	\
    FM_EREPORT_CLASS "." ZFS_ERROR_CLASS "." type


static void zfs_fm_close(zfs_case_t *zcp);

static void
zed_log_fault(nvlist_t *nvl)
{
	nvlist_t *rsrc;
	char *strval;
	uint64_t guid;
	uint8_t byte;

	zed_log_msg(LOG_INFO, "\nzed_fault_event:");

	if (nvlist_lookup_string(nvl, FM_CLASS, &strval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %s", FM_CLASS, strval);
	if (nvlist_lookup_uint8(nvl, FM_FAULT_CERTAINTY, &byte) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %llu", FM_FAULT_CERTAINTY, byte);
	if (nvlist_lookup_nvlist(nvl, FM_FAULT_RESOURCE, &rsrc) == 0) {
		if (nvlist_lookup_string(rsrc, FM_FMRI_SCHEME, &strval) == 0)
			zed_log_msg(LOG_INFO, "\t%s: %s", FM_FMRI_SCHEME,
			    strval);
		if (nvlist_lookup_uint64(rsrc, FM_FMRI_ZFS_POOL, &guid) == 0)
			zed_log_msg(LOG_INFO, "\t%s: %llu", FM_FMRI_ZFS_POOL,
			    guid);
		if (nvlist_lookup_uint64(rsrc, FM_FMRI_ZFS_VDEV, &guid) == 0)
			zed_log_msg(LOG_INFO, "\t%s: %llu \n", FM_FMRI_ZFS_VDEV,
			    guid);
	}
}

static void
fmd_case_add_suspect(zfs_case_t *zcp, nvlist_t *fault)
{
	nvlist_t *nvl;
	int err = 0;

	zed_log_fault(fault);

//	gettimeofday(&cip->ci_tv);

	nvl = fmd_nvl_alloc(FMD_SLEEP);

	err |= nvlist_add_uint8(nvl, FM_VERSION, FM_SUSPECT_VERSION);
	err |= nvlist_add_string(nvl, FM_CLASS, FM_LIST_SUSPECT_CLASS);
//	err |= nvlist_add_string(nvl, FM_SUSPECT_UUID, uuid);
//	err |= nvlist_add_string(nvl, FM_SUSPECT_DIAG_CODE, code);
//	err |= nvlist_add_int64_array(nvl, FM_SUSPECT_DIAG_TIME, tod, 2);
//	err |= nvlist_add_nvlist(nvl, FM_SUSPECT_DE, de_fmri);

	err |= nvlist_add_uint32(nvl, FM_SUSPECT_FAULT_SZ, 1);
	err |= nvlist_add_nvlist_array(nvl, FM_SUSPECT_FAULT_LIST, &fault, 1);

	if (err)
		zed_log_die("failed to populate nvlist");

	zfs_agent_post_event(FM_LIST_SUSPECT_CLASS, NULL, nvl);

	nvlist_free(nvl);
	nvlist_free(fault);
}

static void
fmd_case_solve(zfs_case_t *zcp)
{
	zcp->zc_case_state = FMD_CASE_SOLVED;
}

static int32_t
fmd_prop_get_int32(const char *name)
{
	/* N = 10 events */
	return (10);
}

static int64_t
fmd_prop_get_int64(const char *name)
{
	/* T = 10 minutes */
	return (1000ULL * 1000ULL * 1000ULL * 600ULL);
}

static void
zfs_case_serialize(zfs_case_t *zcp)
{
}

/*
 * Iterate over any active cases.  If any cases are associated with a pool or
 * vdev which is no longer present on the system, close the associated case.
 */
static void
zfs_mark_vdev(uint64_t pool_guid, nvlist_t *vd, er_timeval_t *loaded)
{
	uint64_t vdev_guid;
	uint_t c, children;
	nvlist_t **child;
	zfs_case_t *zcp;
	int ret;

	ret = nvlist_lookup_uint64(vd, ZPOOL_CONFIG_GUID, &vdev_guid);
	assert(ret == 0);

	/*
	 * Mark any cases associated with this (pool, vdev) pair.
	 */
	for (zcp = uu_list_first(zfs_cases); zcp != NULL;
	    zcp = uu_list_next(zfs_cases, zcp)) {
		if (zcp->zc_data.zc_pool_guid == pool_guid &&
		    zcp->zc_data.zc_vdev_guid == vdev_guid) {
			zcp->zc_present = B_TRUE;
			zcp->zc_when = *loaded;
		}
	}

	/*
	 * Iterate over all children.
	 */
	if (nvlist_lookup_nvlist_array(vd, ZPOOL_CONFIG_CHILDREN, &child,
	    &children) == 0) {
		for (c = 0; c < children; c++)
			zfs_mark_vdev(pool_guid, child[c], loaded);
	}

	if (nvlist_lookup_nvlist_array(vd, ZPOOL_CONFIG_L2CACHE, &child,
	    &children) == 0) {
		for (c = 0; c < children; c++)
			zfs_mark_vdev(pool_guid, child[c], loaded);
	}

	if (nvlist_lookup_nvlist_array(vd, ZPOOL_CONFIG_SPARES, &child,
	    &children) == 0) {
		for (c = 0; c < children; c++)
			zfs_mark_vdev(pool_guid, child[c], loaded);
	}
}

/*ARGSUSED*/
static int
zfs_mark_pool(zpool_handle_t *zhp, void *unused)
{
	zfs_case_t *zcp;
	uint64_t pool_guid;
	uint64_t *tod;
	er_timeval_t loaded = { 0 };
	nvlist_t *config, *vd;
	uint_t nelem = 0;
	int ret;

	pool_guid = zpool_get_prop_int(zhp, ZPOOL_PROP_GUID, NULL);
	/*
	 * Mark any cases associated with just this pool.
	 */
	for (zcp = uu_list_first(zfs_cases); zcp != NULL;
	    zcp = uu_list_next(zfs_cases, zcp)) {
		if (zcp->zc_data.zc_pool_guid == pool_guid &&
		    zcp->zc_data.zc_vdev_guid == 0)
			zcp->zc_present = B_TRUE;
	}

	if ((config = zpool_get_config(zhp, NULL)) == NULL) {
		zpool_close(zhp);
		return (-1);
	}

	(void) nvlist_lookup_uint64_array(config, ZPOOL_CONFIG_LOADED_TIME,
	    &tod, &nelem);
	if (nelem == 2) {
		loaded.ertv_sec = tod[0];
		loaded.ertv_nsec = tod[1];
		for (zcp = uu_list_first(zfs_cases); zcp != NULL;
		    zcp = uu_list_next(zfs_cases, zcp)) {
			if (zcp->zc_data.zc_pool_guid == pool_guid &&
			    zcp->zc_data.zc_vdev_guid == 0) {
				zcp->zc_when = loaded;
			}
		}
	}

	ret = nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, &vd);
	assert(ret == 0);

	zfs_mark_vdev(pool_guid, vd, &loaded);

	zpool_close(zhp);

	return (0);
}

struct load_time_arg {
	uint64_t lt_guid;
	er_timeval_t *lt_time;
	boolean_t lt_found;
};

static int
zpool_find_load_time(zpool_handle_t *zhp, void *arg)
{
	struct load_time_arg *lta = arg;
	uint64_t pool_guid;
	uint64_t *tod;
	nvlist_t *config;
	uint_t nelem;

	if (lta->lt_found) {
		zpool_close(zhp);
		return (0);
	}

	pool_guid = zpool_get_prop_int(zhp, ZPOOL_PROP_GUID, NULL);
	if (pool_guid != lta->lt_guid) {
		zpool_close(zhp);
		return (0);
	}

	if ((config = zpool_get_config(zhp, NULL)) == NULL) {
		zpool_close(zhp);
		return (-1);
	}

	if (nvlist_lookup_uint64_array(config, ZPOOL_CONFIG_LOADED_TIME,
	    &tod, &nelem) == 0 && nelem == 2) {
		lta->lt_found = B_TRUE;
		lta->lt_time->ertv_sec = tod[0];
		lta->lt_time->ertv_nsec = tod[1];
	}

	zpool_close(zhp);

	return (0);
}

static void
zfs_purge_cases(void)
{
	zfs_case_t *zcp;
	uu_list_walk_t *walk;

	/*
	 * There is no way to open a pool by GUID, or lookup a vdev by GUID.  No
	 * matter what we do, we're going to have to stomach a O(vdevs * cases)
	 * algorithm.  In reality, both quantities are likely so small that
	 * neither will matter. Given that iterating over pools is more
	 * expensive than iterating over the in-memory case list, we opt for a
	 * 'present' flag in each case that starts off cleared.  We then iterate
	 * over all pools, marking those that are still present, and removing
	 * those that aren't found.
	 *
	 * Note that we could also construct an FMRI and rely on
	 * fmd_nvl_fmri_present(), but this would end up doing the same search.
	 */

	/*
	 * Mark the cases an not present.
	 */
	for (zcp = uu_list_first(zfs_cases); zcp != NULL;
	    zcp = uu_list_next(zfs_cases, zcp))
		zcp->zc_present = B_FALSE;

	/*
	 * Iterate over all pools and mark the pools and vdevs found.  If this
	 * fails (most probably because we're out of memory), then don't close
	 * any of the cases and we cannot be sure they are accurate.
	 */
	if (zpool_iter(g_zfshdl, zfs_mark_pool, NULL) != 0)
		return;

	/*
	 * Remove those cases which were not found.
	 */
	walk = uu_list_walk_start(zfs_cases, UU_WALK_ROBUST);
	while ((zcp = uu_list_walk_next(walk)) != NULL) {
		if (!zcp->zc_present)
			zfs_fm_close(zcp);
	}
	uu_list_walk_end(walk);
}

/*
 * Construct the name of a serd engine given the pool/vdev GUID and type (io or
 * checksum).
 */
static void
zfs_serd_name(char *buf, uint64_t pool_guid, uint64_t vdev_guid,
    const char *type)
{
	(void) snprintf(buf, MAX_SERDLEN, "zfs_%llx_%llx_%s",
	    (long long unsigned int)pool_guid,
	    (long long unsigned int)vdev_guid, type);
}

/*
 * Solve a given ZFS case.  This first checks to make sure the diagnosis is
 * still valid, as well as cleaning up any pending timer associated with the
 * case.
 */
static void
zfs_case_solve(zfs_case_t *zcp, const char *faultname, boolean_t checkunusable)
{
	nvlist_t *detector, *fault;
	boolean_t serialize;
	nvlist_t *fru;
#if 0
	nvlist_t *fmri, *fru;
	topo_hdl_t *thp;
	int err;
#endif

	zed_log_msg(LOG_INFO, DE_TAG"solving fault '%s'", faultname);

	/*
	 * Construct the detector from the case data.  The detector is in the
	 * ZFS scheme, and is either the pool or the vdev, depending on whether
	 * this is a vdev or pool fault.
	 */
	detector = fmd_nvl_alloc(FMD_SLEEP);

	(void) nvlist_add_uint8(detector, FM_VERSION, ZFS_SCHEME_VERSION0);
	(void) nvlist_add_string(detector, FM_FMRI_SCHEME, FM_FMRI_SCHEME_ZFS);
	(void) nvlist_add_uint64(detector, FM_FMRI_ZFS_POOL,
	    zcp->zc_data.zc_pool_guid);
	if (zcp->zc_data.zc_vdev_guid != 0) {
		(void) nvlist_add_uint64(detector, FM_FMRI_ZFS_VDEV,
		    zcp->zc_data.zc_vdev_guid);
	}
	/*
	 * We also want to make sure that the detector (pool or vdev) properly
	 * reflects the diagnosed state, when the fault corresponds to internal
	 * ZFS state (i.e. not checksum or I/O error-induced).  Otherwise, a
	 * device which was unavailable early in boot (because the driver/file
	 * wasn't available) and is now healthy will be mis-diagnosed.
	 */
#if 0
	if (!fmd_nvl_fmri_present(detector) ||
	    (checkunusable && !fmd_nvl_fmri_unusable(detector))) {
		zfs_fm_close(zcp);
		nvlist_free(detector);
		return;
	}
#endif
	fru = NULL;
#if 0
	if (zcp->zc_fru != NULL &&
	    (thp = fmd_hdl_topo_hold(hdl, TOPO_VERSION)) != NULL) {
		/*
		 * If the vdev had an associated FRU, then get the FRU nvlist
		 * from the topo handle and use that in the suspect list.  We
		 * explicitly lookup the FRU because the fmri reported from the
		 * kernel may not have up to date details about the disk itself
		 * (serial, part, etc).
		 */
		if (topo_fmri_str2nvl(thp, zcp->zc_fru, &fmri, &err) == 0) {
			/*
			 * If the disk is part of the system chassis, but the
			 * FRU indicates a different chassis ID than our
			 * current system, then ignore the error.  This
			 * indicates that the device was part of another
			 * cluster head, and for obvious reasons cannot be
			 * imported on this system.
			 */
			if (libzfs_fru_notself(g_zfshdl, zcp->zc_fru)) {
				zfs_fm_close(zcp);
				nvlist_free(fmri);
				fmd_hdl_topo_rele(hdl, thp);
				nvlist_free(detector);
				return;
			}

			/*
			 * If the device is no longer present on the system, or
			 * topo_fmri_fru() fails for other reasons, then fall
			 * back to the fmri specified in the vdev.
			 */
			if (topo_fmri_fru(thp, fmri, &fru, &err) != 0)
				fru = fmd_nvl_dup(hdl, fmri, FMD_SLEEP);
			nvlist_free(fmri);
		}

		fmd_hdl_topo_rele(hdl, thp);
	}
#endif
	fault = fmd_nvl_create_fault(faultname, 100, detector, fru, detector);

	fmd_case_add_suspect(zcp, fault);

	nvlist_free(fru);

	fmd_case_solve(zcp);

	serialize = B_FALSE;
	if (zcp->zc_data.zc_has_remove_timer) {
		fmd_timer_remove(zcp->zc_remove_timer);
		zcp->zc_data.zc_has_remove_timer = 0;
		serialize = B_TRUE;
	}
	if (serialize)
		zfs_case_serialize(zcp);

	nvlist_free(detector);
}

/*
 * The specified case has been closed and any case-specific
 * data structures should be deallocated.
 */
static void
zfs_fm_close(zfs_case_t *zcp)
{
	if (zcp->zc_data.zc_serd_checksum[0] != '\0')
		fmd_serd_destroy(zcp->zc_data.zc_serd_checksum);
	if (zcp->zc_data.zc_serd_io[0] != '\0')
		fmd_serd_destroy(zcp->zc_data.zc_serd_io);
	if (zcp->zc_data.zc_has_remove_timer)
		fmd_timer_remove(zcp->zc_remove_timer);

	uu_list_remove(zfs_cases, zcp);
	uu_list_node_fini(zcp, &zcp->zc_node, zfs_case_pool);
	fmd_free(zcp, sizeof (zfs_case_t));
}

#define	DE_TAG "\x1B[35mDE:\x1B[0m "

/*ARGSUSED*/
int
zfs_diagnosis_init(libzfs_handle_t *zfs_hdl)
{
	if ((g_zfshdl = zfs_hdl) == NULL)
		return (-1);

	zed_log_msg(LOG_INFO, DE_TAG"zfs_diagnosis_init");

	if ((zfs_case_pool = uu_list_pool_create("zfs_case_pool",
	    sizeof (zfs_case_t), offsetof(zfs_case_t, zc_node),
	    NULL, UU_LIST_POOL_DEBUG)) == NULL) {
		return (-1);
	}

	if ((zfs_cases = uu_list_create(zfs_case_pool, NULL,
	    UU_LIST_DEBUG)) == NULL) {
		uu_list_pool_destroy(zfs_case_pool);
		return (-1);
	}

	/* default remove timeout is 15 seconds */
	zfs_remove_timeout = 15ULL * 1000ULL * 1000ULL * 1000ULL;

	fmd_serd_init();

	return (0);
}

/*ARGSUSED*/
void
zfs_diagnosis_fini(void)
{
	zfs_case_t *zcp;
	uu_list_walk_t *walk;

	zed_log_msg(LOG_INFO, DE_TAG"zfs_diagnosis_fini");

	assert(zfs_cases);
	assert(zfs_case_pool);

	/*
	 * Remove all active cases.
	 */
	walk = uu_list_walk_start(zfs_cases, UU_WALK_ROBUST);
	while ((zcp = uu_list_walk_next(walk)) != NULL) {
		zed_log_msg(LOG_INFO, DE_TAG"  removing case ena %llu",
		    (long long unsigned)zcp->zc_data.zc_ena);
#if 1
		zfs_fm_close(zcp);
#else
		uu_list_remove(zfs_cases, zcp);
		fmd_free(zcp, sizeof (zfs_case_t));
#endif
	}
	uu_list_walk_end(walk);

	uu_list_destroy(zfs_cases);
	uu_list_pool_destroy(zfs_case_pool);

	fmd_serd_fini();
	g_zfshdl = NULL;
}

#define	FMD_EVN_TOD	"__tod"

static boolean_t
timeval_earlier(er_timeval_t *a, er_timeval_t *b)
{
	return (a->ertv_sec < b->ertv_sec ||
	    (a->ertv_sec == b->ertv_sec && a->ertv_nsec < b->ertv_nsec));
}

/*ARGSUSED*/
static void
zfs_ereport_when(nvlist_t *nvl, er_timeval_t *when)
{
	uint64_t *tod;
	uint_t  nelem;

	/*
	 * TBD -- can we use 'time' nvpair ?
	 */
	if (nvlist_lookup_uint64_array(nvl, FMD_EVN_TOD, &tod, &nelem) == 0 &&
	    nelem == 2) {
		when->ertv_sec = tod[0];
		when->ertv_nsec = tod[1];
	} else {
		when->ertv_sec = when->ertv_nsec = UINT64_MAX;
	}
}

void
zfs_diagnosis_recv(nvlist_t *nvl, const char *class)
{
	zfs_case_t *zcp, *dcp;
	int32_t pool_state;
	uint64_t ena, pool_guid, vdev_guid;
	er_timeval_t pool_load;
	er_timeval_t er_when;
	struct timespec ts = {0};
	nvlist_t *detector;
	boolean_t pool_found = B_FALSE;
	boolean_t isresource;
	char *type;
	int64_t *tv;
	uint_t n;

	/*
	 * We subscribe to notifications for vdev or pool removal.  In these
	 * cases, there may be cases that no longer apply.  Purge any cases
	 * that no longer apply.
	 */
	if (fmd_nvl_class_match(nvl, "resource.sysevent.EC_zfs.*")) {
		zfs_purge_cases();
		zfs_stats.resource_drops++;
		return;
	}

	isresource = fmd_nvl_class_match(nvl, "resource.fs.zfs.*");

	if (isresource) {
		/*
		 * For resources, we don't have a normal payload.
		 */
		if (nvlist_lookup_uint64(nvl, FM_EREPORT_PAYLOAD_ZFS_VDEV_GUID,
		    &vdev_guid) != 0)
			pool_state = SPA_LOAD_OPEN;
		else
			pool_state = SPA_LOAD_NONE;
		detector = NULL;
	} else {
		(void) nvlist_lookup_nvlist(nvl,
		    FM_EREPORT_DETECTOR, &detector);
		(void) nvlist_lookup_int32(nvl,
		    FM_EREPORT_PAYLOAD_ZFS_POOL_CONTEXT, &pool_state);
	}

	/*
	 * We also ignore all ereports generated during an import of a pool,
	 * since the only possible fault (.pool) would result in import failure,
	 * and hence no persistent fault.  Some day we may want to do something
	 * with these ereports, so we continue generating them internally.
	 */
	if (pool_state == SPA_LOAD_IMPORT) {
		zfs_stats.import_drops++;
		zed_log_msg(LOG_INFO, DE_TAG"zfs_diagnosis_recv: Ignore %s "
		    "during import", class);
		return;
	}

	/*
	 * Device I/O errors are ignored during pool open.
	 */
	if (pool_state == SPA_LOAD_OPEN &&
	    (fmd_nvl_class_match(nvl, "ereport.fs.zfs.checksum") ||
	    fmd_nvl_class_match(nvl, "ereport.fs.zfs.io") ||
	    fmd_nvl_class_match(nvl, "ereport.fs.zfs.probe_failure"))) {
		zfs_stats.dev_drops++;
		zed_log_msg(LOG_INFO, DE_TAG"zfs_diagnosis_recv: Ignore %s "
		    "during pool open", class);
		return;
	}

	/*
	 * We ignore ereports for anything except disks and files.
	 */
	if (nvlist_lookup_string(nvl, FM_EREPORT_PAYLOAD_ZFS_VDEV_TYPE,
	    &type) == 0) {
		if (strcmp(type, VDEV_TYPE_DISK) != 0 &&
		    strcmp(type, VDEV_TYPE_FILE) != 0) {
			zfs_stats.vdev_drops++;
#if 0
			zed_log_msg(LOG_INFO, "zfs_diagnosis_recv: Ignore %s "
			    "not a disk", class);
#endif
			return;
		}
	}

	zed_log_msg(LOG_INFO, DE_TAG"zfs_diagnosis_recv: %s", class);

	/*
	 * Determine if this ereport corresponds to an open case.
	 * Each vdev or pool can have a single case.
	 */
	(void) nvlist_lookup_uint64(nvl,
	    FM_EREPORT_PAYLOAD_ZFS_POOL_GUID, &pool_guid);
	if (nvlist_lookup_uint64(nvl,
	    FM_EREPORT_PAYLOAD_ZFS_VDEV_GUID, &vdev_guid) != 0)
		vdev_guid = 0;
	if (nvlist_lookup_uint64(nvl, FM_EREPORT_ENA, &ena) != 0)
		ena = 0;
	if (nvlist_lookup_int64_array(nvl, FM_EREPORT_TIME, &tv, &n) == 0) {
		ts.tv_sec = tv[0];
		ts.tv_nsec = tv[1];

	}

	zfs_ereport_when(nvl, &er_when);

	for (zcp = uu_list_first(zfs_cases); zcp != NULL;
	    zcp = uu_list_next(zfs_cases, zcp)) {
		if (zcp->zc_data.zc_pool_guid == pool_guid) {
			pool_found = B_TRUE;
			pool_load = zcp->zc_when;
		}
		if (zcp->zc_data.zc_vdev_guid == vdev_guid)
			break;
	}

	if (pool_found) {
#if 0
		zed_log_msg(LOG_INFO, "pool %llx, "
		    "ereport time %lld.%lld, pool load time = %lld.%lld\n",
		    pool_guid, er_when.ertv_sec, er_when.ertv_nsec,
		    pool_load.ertv_sec, pool_load.ertv_nsec);
#endif
	}

	/*
	 * Avoid falsely accusing a pool of being faulty.  Do so by
	 * not replaying ereports that were generated prior to the
	 * current import.  If the failure that generated them was
	 * transient because the device was actually removed but we
	 * didn't receive the normal asynchronous notification, we
	 * don't want to mark it as faulted and potentially panic. If
	 * there is still a problem we'd expect not to be able to
	 * import the pool, or that new ereports will be generated
	 * once the pool is used.
	 */
	if (pool_found && timeval_earlier(&er_when, &pool_load)) {
		zfs_stats.old_drops++;
		return;
	}

	if (!pool_found) {
		/*
		 * Haven't yet seen this pool, but same situation
		 * may apply.
		 */
		struct load_time_arg la;

		la.lt_guid = pool_guid;
		la.lt_time = &pool_load;
		la.lt_found = B_FALSE;

		if (zpool_iter(g_zfshdl, zpool_find_load_time, &la) == 0 &&
		    la.lt_found == B_TRUE) {
			pool_found = B_TRUE;
#if 0
			zed_log_msg(LOG_INFO, "pool %llx, "
			    "ereport time %lld.%lld, "
			    "pool load time = %lld.%lld\n",
			    pool_guid, er_when.ertv_sec, er_when.ertv_nsec,
			    pool_load.ertv_sec, pool_load.ertv_nsec);
#endif
			if (timeval_earlier(&er_when, &pool_load)) {
				zfs_stats.old_drops++;
				return;
			}
		}
	}

	if (zcp == NULL) {
//		fmd_case_t *cs;
//		zfs_case_data_t data = { 0 };

		/*
		 * If this is one of our 'fake' resource ereports, and there is
		 * no case open, simply discard it.
		 */
		if (isresource) {
			zfs_stats.resource_drops++;
			return;
		}

		/*
		 * Open a new case.
		 */
		zed_log_msg(LOG_INFO, DE_TAG"opening case on pool %llu",
		    pool_guid);
//		cs = fmd_case_open(hdl, NULL);

		/*
		 * Initialize the case buffer.  To commonize code, we actually
		 * create the buffer with existing data, and then call
		 * zfs_case_unserialize() to instantiate the in-core structure.
		 */
//		fmd_buf_create(hdl, cs, CASE_DATA, sizeof (zfs_case_data_t));

		zcp = fmd_zalloc(sizeof (zfs_case_t), FMD_SLEEP);

		zcp->zc_data.zc_version = CASE_DATA_VERSION_SERD;
		zcp->zc_case_state = FMD_CASE_UNSOLVED;
		zcp->zc_data.zc_ena = ena;
		zcp->zc_data.zc_pool_guid = pool_guid;
		zcp->zc_data.zc_vdev_guid = vdev_guid;
		zcp->zc_data.zc_pool_state = (int)pool_state;

		uu_list_node_init(zcp, &zcp->zc_node, zfs_case_pool);
#if 1
		if (zcp->zc_data.zc_has_remove_timer)
			zcp->zc_remove_timer = fmd_timer_install(zcp, NULL,
			    zfs_remove_timeout);
#else
		fmd_buf_write(hdl, cs, CASE_DATA, &data, sizeof (data));
		zcp = zfs_case_unserialize(hdl, cs);
#endif
		assert(zcp != NULL);

		(void) uu_list_insert_before(zfs_cases, NULL, zcp);

		if (pool_found)
			zcp->zc_when = pool_load;
	}

	if (isresource) {
		if (fmd_nvl_class_match(nvl, "resource.fs.zfs.autoreplace")) {
			/*
			 * The 'resource.fs.zfs.autoreplace' event indicates
			 * that the pool was loaded with the 'autoreplace'
			 * property set.  In this case, any pending device
			 * failures should be ignored, as the asynchronous
			 * autoreplace handling will take care of them.
			 */
			zfs_fm_close(zcp);
		} else if (fmd_nvl_class_match(nvl,
		    "resource.fs.zfs.removed")) {
			/*
			 * The 'resource.fs.zfs.removed' event indicates that
			 * device removal was detected, and the device was
			 * closed asynchronously.  If this is the case, we
			 * assume that any recent I/O errors were due to the
			 * device removal, not any fault of the device itself.
			 * We reset the SERD engine, and cancel any pending
			 * timers.
			 */
			if (zcp->zc_data.zc_has_remove_timer) {
				fmd_timer_remove(zcp->zc_remove_timer);
				zcp->zc_data.zc_has_remove_timer = 0;
				zfs_case_serialize(zcp);
			}
			if (zcp->zc_data.zc_serd_io[0] != '\0')
				fmd_serd_reset(zcp->zc_data.zc_serd_io);
			if (zcp->zc_data.zc_serd_checksum[0] != '\0')
				fmd_serd_reset(zcp->zc_data.zc_serd_checksum);
		}
		zfs_stats.resource_drops++;
		return;
	}

	/*
	 * Don't do anything else if this case is already solved.
	 */
	if (zcp->zc_case_state >= FMD_CASE_SOLVED)
		return;

	/*
	 * Determine if we should solve the case and generate a fault.  We solve
	 * a case if:
	 *
	 *	a. A pool failed to open (ereport.fs.zfs.pool)
	 *	b. A device failed to open (ereport.fs.zfs.pool) while a pool
	 *	   was up and running.
	 *
	 * We may see a series of ereports associated with a pool open, all
	 * chained together by the same ENA.  If the pool open succeeds, then
	 * we'll see no further ereports.  To detect when a pool open has
	 * succeeded, we associate a timer with the event.  When it expires, we
	 * close the case.
	 */
	if (fmd_nvl_class_match(nvl,
	    ZFS_MAKE_EREPORT(FM_EREPORT_ZFS_POOL))) {
		/*
		 * Pool level fault.  Before solving the case, go through and
		 * close any open device cases that may be pending.
		 */
		for (dcp = uu_list_first(zfs_cases); dcp != NULL;
		    dcp = uu_list_next(zfs_cases, dcp)) {
			if (dcp->zc_data.zc_pool_guid ==
			    zcp->zc_data.zc_pool_guid &&
			    dcp->zc_data.zc_vdev_guid != 0)
				zfs_fm_close(dcp);
		}

		zfs_case_solve(zcp, "fault.fs.zfs.pool", B_TRUE);
	} else if (fmd_nvl_class_match(nvl,
	    ZFS_MAKE_EREPORT(FM_EREPORT_ZFS_LOG_REPLAY))) {
		/*
		 * Pool level fault for reading the intent logs.
		 */
		zfs_case_solve(zcp, "fault.fs.zfs.log_replay", B_TRUE);
	} else if (fmd_nvl_class_match(nvl, "ereport.fs.zfs.vdev.*")) {
		/*
		 * Device fault.
		 */
		zfs_case_solve(zcp, "fault.fs.zfs.device",  B_TRUE);
	} else if (fmd_nvl_class_match(nvl,
	    ZFS_MAKE_EREPORT(FM_EREPORT_ZFS_IO)) ||
	    fmd_nvl_class_match(nvl,
	    ZFS_MAKE_EREPORT(FM_EREPORT_ZFS_CHECKSUM)) ||
	    fmd_nvl_class_match(nvl,
	    ZFS_MAKE_EREPORT(FM_EREPORT_ZFS_IO_FAILURE)) ||
	    fmd_nvl_class_match(nvl,
	    ZFS_MAKE_EREPORT(FM_EREPORT_ZFS_PROBE_FAILURE))) {
		char *failmode = NULL;
		boolean_t checkremove = B_FALSE;

		/*
		 * If this is a checksum or I/O error, then toss it into the
		 * appropriate SERD engine and check to see if it has fired.
		 * Ideally, we want to do something more sophisticated,
		 * (persistent errors for a single data block, etc).  For now,
		 * a single SERD engine is sufficient.
		 */
		if (fmd_nvl_class_match(nvl,
		    ZFS_MAKE_EREPORT(FM_EREPORT_ZFS_IO))) {
			if (zcp->zc_data.zc_serd_io[0] == '\0') {
				zfs_serd_name(zcp->zc_data.zc_serd_io,
				    pool_guid, vdev_guid, "io");

				fmd_serd_create(zcp->zc_data.zc_serd_io,
				    fmd_prop_get_int32("io_N"),
				    fmd_prop_get_int64("io_T"));
				zfs_case_serialize(zcp);
			}
			if (fmd_serd_record(zcp->zc_data.zc_serd_io, ena, &ts))
				checkremove = B_TRUE;
		} else if (fmd_nvl_class_match(nvl,
		    ZFS_MAKE_EREPORT(FM_EREPORT_ZFS_CHECKSUM))) {
			if (zcp->zc_data.zc_serd_checksum[0] == '\0') {
				zfs_serd_name(zcp->zc_data.zc_serd_checksum,
				    pool_guid, vdev_guid, "checksum");

				fmd_serd_create(zcp->zc_data.zc_serd_checksum,
				    fmd_prop_get_int32("checksum_N"),
				    fmd_prop_get_int64("checksum_T"));
				zfs_case_serialize(zcp);
			}
			if (fmd_serd_record(zcp->zc_data.zc_serd_checksum, ena,
			    &ts)) {
				zfs_case_solve(zcp,
				    "fault.fs.zfs.vdev.checksum", B_FALSE);
			}
		} else if (fmd_nvl_class_match(nvl,
		    ZFS_MAKE_EREPORT(FM_EREPORT_ZFS_IO_FAILURE)) &&
		    (nvlist_lookup_string(nvl,
		    FM_EREPORT_PAYLOAD_ZFS_POOL_FAILMODE, &failmode) == 0) &&
		    failmode != NULL) {
			if (strncmp(failmode, FM_EREPORT_FAILMODE_CONTINUE,
			    strlen(FM_EREPORT_FAILMODE_CONTINUE)) == 0) {
				zfs_case_solve(zcp,
				    "fault.fs.zfs.io_failure_continue",
				    B_FALSE);
			} else if (strncmp(failmode, FM_EREPORT_FAILMODE_WAIT,
			    strlen(FM_EREPORT_FAILMODE_WAIT)) == 0) {
				zfs_case_solve(zcp,
				    "fault.fs.zfs.io_failure_wait", B_FALSE);
			}
		} else if (fmd_nvl_class_match(nvl,
		    ZFS_MAKE_EREPORT(FM_EREPORT_ZFS_PROBE_FAILURE))) {
			checkremove = B_TRUE;
		}

		/*
		 * Because I/O errors may be due to device removal, we postpone
		 * any diagnosis until we're sure that we aren't about to
		 * receive a 'resource.fs.zfs.removed' event.
		 */
		if (checkremove) {
			if (zcp->zc_data.zc_has_remove_timer)
				fmd_timer_remove(zcp->zc_remove_timer);
			zcp->zc_remove_timer = fmd_timer_install(zcp, NULL,
			    zfs_remove_timeout);
			if (!zcp->zc_data.zc_has_remove_timer) {
				zcp->zc_data.zc_has_remove_timer = 1;
				zfs_case_serialize(zcp);
			}
		}
	}
}

/*
 * The timeout is fired when we diagnosed an I/O error, and it was not due to
 * device removal (which would cause the timeout to be cancelled).
 */
/* ARGSUSED */
static void
zfs_fm_timeout(timer_t id, void *data)
{
	zfs_case_t *zcp = data;

	zed_log_msg(LOG_INFO, DE_TAG"timeout fired (%p)", id);

	if (id == zcp->zc_remove_timer)
		zfs_case_solve(zcp, "fault.fs.zfs.vdev.io", B_FALSE);
}



static void *
fmd_zalloc(size_t size, int flags)
{
	void *data = malloc(size);

	if (data != NULL)
		bzero(data, size);

	return (data);
}

static void
fmd_free(void *data, size_t size)
{
	free(data);
}

static int
fmd_strmatch(const char *s, const char *p)
{
	char c;

	if (p == NULL)
		return (0);

	if (s == NULL)
		s = ""; /* treat NULL string as the empty string */

	do {
		if ((c = *p++) == '\0')
			return (*s == '\0');

		if (c == '*') {
			while (*p == '*')
				p++; /* consecutive *'s can be collapsed */

			if (*p == '\0')
				return (1);

			while (*s != '\0') {
				if (fmd_strmatch(s++, p) != 0)
					return (1);
			}

			return (0);
		}
	} while (c == *s++);

	return (0);
}

int
fmd_nvl_class_match(nvlist_t *nvl, const char *pattern)
{
	char *class;

	return (nvl != NULL &&
	    nvlist_lookup_string(nvl, FM_CLASS, &class) == 0 &&
	    fmd_strmatch(class, pattern));
}

static nvlist_t *
fmd_nvl_alloc(int flags)
{
	nvlist_t *nvl = NULL;

	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0) != 0)
		return (NULL);

	return (nvl);
}

static nvlist_t *
fmd_nvl_create_fault(const char *class, uint8_t certainty, nvlist_t *asru,
    nvlist_t *fru, nvlist_t *resource)
{
	nvlist_t *nvl;
	int err = 0;

	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0) != 0)
		zed_log_die("failed to xalloc fault nvlist");

	err |= nvlist_add_uint8(nvl, FM_VERSION, FM_FAULT_VERSION);
	err |= nvlist_add_string(nvl, FM_CLASS, class);
	err |= nvlist_add_uint8(nvl, FM_FAULT_CERTAINTY, certainty);

	if (asru != NULL)
		err |= nvlist_add_nvlist(nvl, FM_FAULT_ASRU, asru);
	if (fru != NULL)
		err |= nvlist_add_nvlist(nvl, FM_FAULT_FRU, fru);
	if (resource != NULL)
		err |= nvlist_add_nvlist(nvl, FM_FAULT_RESOURCE, resource);

	if (err)
		zed_log_die("failed to populate nvlist: %s\n", strerror(err));

	return (nvl);
}


#define	_POSIC_C_SOURCE	199309
#include <signal.h>
#include <time.h>

// Link with -lrt.


static void
_timer_notify(union sigval sv)
{
	zfs_case_t *zcp = sv.sival_ptr;

	zfs_fm_timeout(zcp->zc_remove_timer, zcp);
}

/* delta is in nanosecs */
static timer_t
fmd_timer_install(void *arg, fmd_event_t *ep, hrtime_t delta)
{
	struct sigevent sev;
	struct itimerspec its;
	timer_t timer_id;

	its.it_value.tv_sec = delta / 1000000000;
	its.it_value.tv_nsec = delta % 1000000000;
	its.it_interval.tv_sec = its.it_value.tv_sec;
	its.it_interval.tv_nsec = its.it_value.tv_nsec;

	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_notify_function = _timer_notify;
	sev.sigev_notify_attributes = NULL;
	sev.sigev_value.sival_ptr = arg;	/* zfs case ptr */

	zed_log_msg(LOG_INFO, DE_TAG"installing timer for %d secs (%p)",
	    (int)its.it_value.tv_sec, timer_id);

	timer_create(CLOCK_REALTIME, &sev, &timer_id);

	timer_settime(timer_id, 0, &its, NULL);

	return (timer_id);
}

static void
fmd_timer_remove(timer_t id)
{
	zed_log_msg(LOG_INFO, DE_TAG"removing timer %p", id);

	timer_delete(id);
}
