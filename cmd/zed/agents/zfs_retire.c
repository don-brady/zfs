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
 * Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.
 */

/*
 * The ZFS retire agent is responsible for managing hot spares across all pools.
 * When we see a device fault or a device removal, we try to open the associated
 * pool and look for any hot spares.  We iterate over any available hot spares
 * and attempt a 'zpool replace' for each one.
 *
 * For vdevs diagnosed as faulty, the agent is also responsible for proactively
 * marking the vdev FAULTY (for I/O errors) or DEGRADED (for checksum errors).
 */

#include <sys/fs/zfs.h>
#include <sys/fm/protocol.h>
#include <sys/fm/fs/zfs.h>
#include <libnvpair.h>
#include <libzfs.h>
#include <stdlib.h>
#include <string.h>

#include "zfs_agents.h"
#include "../zed_log.h"

#define	RA_TAG "\x1B[32m\t\t\t\tRA:\x1B[0m "

extern int fmd_nvl_class_match(nvlist_t *nvl, const char *pattern);

static int32_t
fmd_prop_get_int32(const char *name)
{
	return (10);
}

typedef struct zfs_retire_repaired {
	struct zfs_retire_repaired	*zrr_next;
	uint64_t			zrr_pool;
	uint64_t			zrr_vdev;
} zfs_retire_repaired_t;

typedef struct zfs_retire_data {
	libzfs_handle_t			*zrd_hdl;
	zfs_retire_repaired_t		*zrd_repaired;
} zfs_retire_data_t;

zfs_retire_data_t g_zrd;

static void
zfs_retire_clear_data(zfs_retire_data_t *zdp)
{
	zfs_retire_repaired_t *zrp;

	while ((zrp = zdp->zrd_repaired) != NULL) {
		zdp->zrd_repaired = zrp->zrr_next;
		free(zrp);
	}
}

/*
 * Find a pool with a matching GUID.
 */
typedef struct find_cbdata {
	uint64_t	cb_guid;
	const char	*cb_fru;
	zpool_handle_t	*cb_zhp;
	nvlist_t	*cb_vdev;
} find_cbdata_t;

static int
find_pool(zpool_handle_t *zhp, void *data)
{
	find_cbdata_t *cbp = data;

	if (cbp->cb_guid ==
	    zpool_get_prop_int(zhp, ZPOOL_PROP_GUID, NULL)) {
		cbp->cb_zhp = zhp;
		return (1);
	}

	zpool_close(zhp);
	return (0);
}

/*
 * Find a vdev within a tree with a matching GUID.
 */
static nvlist_t *
find_vdev(libzfs_handle_t *zhdl, nvlist_t *nv, const char *search_fru,
    uint64_t search_guid)
{
	uint64_t guid;
	nvlist_t **child;
	uint_t c, children;
	nvlist_t *ret;
	char *fru;

	if (search_fru != NULL) {
		if (nvlist_lookup_string(nv, ZPOOL_CONFIG_FRU, &fru) == 0 &&
		    libzfs_fru_compare(zhdl, fru, search_fru))
			return (nv);
	} else {
		if (nvlist_lookup_uint64(nv, ZPOOL_CONFIG_GUID, &guid) == 0 &&
		    guid == search_guid) {
			zed_log_msg(LOG_INFO, RA_TAG"found vdev %llu", guid);
			return (nv);
		}
	}

	if (nvlist_lookup_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN,
	    &child, &children) != 0)
		return (NULL);

	for (c = 0; c < children; c++) {
		if ((ret = find_vdev(zhdl, child[c], search_fru,
		    search_guid)) != NULL)
			return (ret);
	}

	if (nvlist_lookup_nvlist_array(nv, ZPOOL_CONFIG_L2CACHE,
	    &child, &children) != 0)
		return (NULL);

	for (c = 0; c < children; c++) {
		if ((ret = find_vdev(zhdl, child[c], search_fru,
		    search_guid)) != NULL)
			return (ret);
	}

	return (NULL);
}


/*
 * Given a (pool, vdev) GUID pair, find the matching pool and vdev.
 */
static zpool_handle_t *
find_by_guid(libzfs_handle_t *zhdl, uint64_t pool_guid, uint64_t vdev_guid,
    nvlist_t **vdevp)
{
	find_cbdata_t cb;
	zpool_handle_t *zhp;
	nvlist_t *config, *nvroot;

	/*
	 * Find the corresponding pool and make sure the vdev still exists.
	 */
	cb.cb_guid = pool_guid;
	if (zpool_iter(zhdl, find_pool, &cb) != 1)
		return (NULL);

	zhp = cb.cb_zhp;
	config = zpool_get_config(zhp, NULL);
	if (nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE,
	    &nvroot) != 0) {
		zpool_close(zhp);
		return (NULL);
	}

	if (vdev_guid != 0) {
		if ((*vdevp = find_vdev(zhdl, nvroot, NULL,
		    vdev_guid)) == NULL) {
			zpool_close(zhp);
			return (NULL);
		}
	}

	return (zhp);
}

/*
 * Given a vdev, attempt to replace it with every known spare until one
 * succeeds.
 */
static void
replace_with_spare(zpool_handle_t *zhp, nvlist_t *vdev)
{
	nvlist_t *config, *nvroot, *replacement;
	nvlist_t **spares;
	uint_t s, nspares;
	char *dev_name;

	config = zpool_get_config(zhp, NULL);
	if (nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE,
	    &nvroot) != 0)
		return;

	/*
	 * Find out if there are any hot spares available in the pool.
	 */
	if (nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_SPARES,
	    &spares, &nspares) != 0)
		return;

	if (nvlist_alloc(&replacement, NV_UNIQUE_NAME, 0) != 0) {
		zed_log_msg(LOG_WARNING, "nvlist_alloc: out of memory");
		return;
	}

	(void) nvlist_add_string(replacement, ZPOOL_CONFIG_TYPE,
	    VDEV_TYPE_ROOT);

	dev_name = zpool_vdev_name(NULL, zhp, vdev, 0);

	/*
	 * Try to replace each spare, ending when we successfully
	 * replace it.
	 */
	for (s = 0; s < nspares; s++) {
		char *spare_name;

		if (nvlist_lookup_string(spares[s], ZPOOL_CONFIG_PATH,
		    &spare_name) != 0)
			continue;

		(void) nvlist_add_nvlist_array(replacement,
		    ZPOOL_CONFIG_CHILDREN, &spares[s], 1);

		zed_log_msg(LOG_INFO, RA_TAG"zpool_vdev_replace '%s' with "
		    "spare '%s'", dev_name, spare_name);

		if (zpool_vdev_attach(zhp, dev_name, spare_name,
		    replacement, B_TRUE) == 0)
			break;
	}

	free(dev_name);
	nvlist_free(replacement);
}

/*
 * Repair this vdev if we had diagnosed a 'fault.fs.zfs.device' and
 * ASRU is now usable.  ZFS has found the device to be present and
 * functioning.
 */
static void
zfs_vdev_repair(nvlist_t *nvl)
{
	zfs_retire_repaired_t *zrp;
	uint64_t pool_guid, vdev_guid;
	nvlist_t *asru;

	if (nvlist_lookup_uint64(nvl, FM_EREPORT_PAYLOAD_ZFS_POOL_GUID,
	    &pool_guid) != 0 || nvlist_lookup_uint64(nvl,
	    FM_EREPORT_PAYLOAD_ZFS_VDEV_GUID, &vdev_guid) != 0)
		return;

	/*
	 * Before checking the state of the ASRU, go through and see if we've
	 * already made an attempt to repair this ASRU.  This list is cleared
	 * whenever we receive any kind of list event, and is designed to
	 * prevent us from generating a feedback loop when we attempt repairs
	 * against a faulted pool.  The problem is that checking the unusable
	 * state of the ASRU can involve opening the pool, which can post
	 * statechange events but otherwise leave the pool in the faulted
	 * state.  This list allows us to detect when a statechange event is
	 * due to our own request.
	 */
	for (zrp = g_zrd.zrd_repaired; zrp != NULL; zrp = zrp->zrr_next) {
		if (zrp->zrr_pool == pool_guid &&
		    zrp->zrr_vdev == vdev_guid)
			return;
	}

	if (nvlist_alloc(&asru, NV_UNIQUE_NAME, 0) != 0)
		return;


	(void) nvlist_add_uint8(asru, FM_VERSION, ZFS_SCHEME_VERSION0);
	(void) nvlist_add_string(asru, FM_FMRI_SCHEME, FM_FMRI_SCHEME_ZFS);
	(void) nvlist_add_uint64(asru, FM_FMRI_ZFS_POOL, pool_guid);
	(void) nvlist_add_uint64(asru, FM_FMRI_ZFS_VDEV, vdev_guid);

	/*
	 * We explicitly check for the unusable state here to make sure we
	 * aren't responding to a transient state change.  As part of opening a
	 * vdev, it's possible to see the 'statechange' event, only to be
	 * followed by a vdev failure later.  If we don't check the current
	 * state of the vdev (or pool) before marking it repaired, then we risk
	 * generating spurious repair events followed immediately by the same
	 * diagnosis.
	 *
	 * This assumes that the ZFS scheme code associated unusable (i.e.
	 * isolated) with its own definition of faulty state.  In the case of a
	 * DEGRADED leaf vdev (due to checksum errors), this is not the case.
	 * This works, however, because the transient state change is not
	 * posted in this case.  This could be made more explicit by not
	 * relying on the scheme's unusable callback and instead directly
	 * checking the vdev state, where we could correctly account for
	 * DEGRADED state.
	 */
#if 0
	if (!fmd_nvl_fmri_unusable(hdl, asru) && fmd_nvl_fmri_has_fault(hdl,
	    asru, FMD_HAS_FAULT_ASRU, NULL)) {
		topo_hdl_t *thp;
		char *fmri = NULL;
		int err;

		thp = fmd_hdl_topo_hold(hdl, TOPO_VERSION);
		if (topo_fmri_nvl2str(thp, asru, &fmri, &err) == 0)
			(void) fmd_repair_asru(hdl, fmri);
		fmd_hdl_topo_rele(hdl, thp);

		topo_hdl_strfree(thp, fmri);
	}
#endif
	nvlist_free(asru);
	zrp = malloc(sizeof (zfs_retire_repaired_t));
	zrp->zrr_next = g_zrd.zrd_repaired;
	zrp->zrr_pool = pool_guid;
	zrp->zrr_vdev = vdev_guid;
	g_zrd.zrd_repaired = zrp;

	zed_log_msg(LOG_INFO, RA_TAG"repaired vdev %llu on pool %llu",
	    vdev_guid, pool_guid);
}

void
zfs_retire_recv(nvlist_t *nvl, const char *class)
{
	uint64_t pool_guid, vdev_guid;
	zpool_handle_t *zhp;
	nvlist_t *resource, *fault, *fru;
	nvlist_t **faults;
	uint_t f, nfaults;
	zfs_retire_data_t *zdp = &g_zrd;
	libzfs_handle_t *zhdl = zdp->zrd_hdl;
	boolean_t fault_device, degrade_device;
	boolean_t is_repair;
	char *scheme;
	nvlist_t *vdev = NULL;
//	char *uuid;
	int repair_done = 0;
	boolean_t retire;
	boolean_t is_disk;
	vdev_aux_t aux;
//	topo_hdl_t *thp;
//	int err;
	uint64_t state = 0;

	zed_log_msg(LOG_INFO, RA_TAG"zfs_retire_recv: '%s'", class);

	/*
	 * If this is a resource notifying us of device removal, then simply
	 * check for an available spare and continue.
	 */
	if (strcmp(class, "resource.fs.zfs.removed") == 0) {
		if (nvlist_lookup_uint64(nvl, FM_EREPORT_PAYLOAD_ZFS_POOL_GUID,
		    &pool_guid) != 0 ||
		    nvlist_lookup_uint64(nvl, FM_EREPORT_PAYLOAD_ZFS_VDEV_GUID,
		    &vdev_guid) != 0)
			return;

		if ((zhp = find_by_guid(zhdl, pool_guid, vdev_guid,
		    &vdev)) == NULL)
			return;

		if (fmd_prop_get_int32("spare_on_remove"))
			replace_with_spare(zhp, vdev);
		zpool_close(zhp);
		return;
	}

	if (strcmp(class, FM_LIST_RESOLVED_CLASS) == 0)
		return;

	/*
	 * Note: on zfsonlinux statechange transition are more than just
	 * healthy ones so we need to confim the actual state value.
	 */
	if (strcmp(class, "resource.fs.zfs.statechange") == 0 &&
	    nvlist_lookup_uint64(nvl, FM_EREPORT_PAYLOAD_ZFS_VDEV_STATE,
	    &state) == 0 && state == VDEV_STATE_HEALTHY) {;
		zfs_vdev_repair(nvl);
		return;
	}

	/*
	 * TBD: confim actual class name for ESC_ZFS_VDEV_REMOVE
	 */
	if (strcmp(class, "resource.sysevent.vdev_remove") == 0) {
		zfs_vdev_repair(nvl);
		return;
	}

	zfs_retire_clear_data(zdp);

	if (strcmp(class, FM_LIST_REPAIRED_CLASS) == 0)
		is_repair = B_TRUE;
	else
		is_repair = B_FALSE;

	/*
	 * We subscribe to zfs faults as well as all repair events.
	 */
	if (nvlist_lookup_nvlist_array(nvl, FM_SUSPECT_FAULT_LIST,
	    &faults, &nfaults) != 0)
		return;

	for (f = 0; f < nfaults; f++) {
		fault = faults[f];

		fault_device = B_FALSE;
		degrade_device = B_FALSE;
		is_disk = B_FALSE;

		if (nvlist_lookup_boolean_value(fault, FM_SUSPECT_RETIRE,
		    &retire) == 0 && retire == 0)
			continue;

		/*
		 * While we subscribe to fault.fs.zfs.*, we only take action
		 * for faults targeting a specific vdev (open failure or SERD
		 * failure).  We also subscribe to fault.io.* events, so that
		 * faulty disks will be faulted in the ZFS configuration.
		 */
		if (fmd_nvl_class_match(fault, "fault.fs.zfs.vdev.io")) {
			fault_device = B_TRUE;
		} else if (fmd_nvl_class_match(fault,
		    "fault.fs.zfs.vdev.checksum")) {
			degrade_device = B_TRUE;
		} else if (fmd_nvl_class_match(fault,
		    "fault.fs.zfs.device")) {
			fault_device = B_FALSE;
		} else if (fmd_nvl_class_match(fault, "fault.io.*")) {
			is_disk = B_TRUE;
			fault_device = B_TRUE;
		} else {
			continue;
		}

		if (is_disk) {
			/*
			 * This is a disk fault.  Lookup the FRU, convert it to
			 * an FMRI string, and attempt to find a matching vdev.
			 */
			if (nvlist_lookup_nvlist(fault, FM_FAULT_FRU,
			    &fru) != 0 ||
			    nvlist_lookup_string(fru, FM_FMRI_SCHEME,
			    &scheme) != 0)
				continue;

			if (strcmp(scheme, FM_FMRI_SCHEME_HC) != 0)
				continue;
#if 0
			thp = fmd_hdl_topo_hold(hdl, TOPO_VERSION);
			if (topo_fmri_nvl2str(thp, fru, &fmri, &err) != 0) {
				fmd_hdl_topo_rele(hdl, thp);
				continue;
			}

			zhp = find_by_fru(zhdl, fmri, &vdev);
			topo_hdl_strfree(thp, fmri);
			fmd_hdl_topo_rele(hdl, thp);

			if (zhp == NULL)
				continue;

			(void) nvlist_lookup_uint64(vdev,
			    ZPOOL_CONFIG_GUID, &vdev_guid);
			aux = VDEV_AUX_EXTERNAL;
#endif
			continue;
		} else {
			/*
			 * This is a ZFS fault.  Lookup the resource, and
			 * attempt to find the matching vdev.
			 */
			if (nvlist_lookup_nvlist(fault, FM_FAULT_RESOURCE,
			    &resource) != 0 ||
			    nvlist_lookup_string(resource, FM_FMRI_SCHEME,
			    &scheme) != 0)
				continue;

			if (strcmp(scheme, FM_FMRI_SCHEME_ZFS) != 0)
				continue;

			if (nvlist_lookup_uint64(resource, FM_FMRI_ZFS_POOL,
			    &pool_guid) != 0)
				continue;

			if (nvlist_lookup_uint64(resource, FM_FMRI_ZFS_VDEV,
			    &vdev_guid) != 0) {
				if (is_repair)
					vdev_guid = 0;
				else
					continue;
			}

			if ((zhp = find_by_guid(zhdl, pool_guid, vdev_guid,
			    &vdev)) == NULL)
				continue;

			aux = VDEV_AUX_ERR_EXCEEDED;
		}

		if (vdev_guid == 0) {
			/*
			 * For pool-level repair events, clear the entire pool.
			 */
			zed_log_msg(LOG_INFO, RA_TAG"zpool_clear of pool '%s'",
			    zpool_get_name(zhp));
			(void) zpool_clear(zhp, NULL, NULL);
			zpool_close(zhp);
			continue;
		}
		/*
		 * If this is a repair event, then mark the vdev as repaired and
		 * continue.
		 */
		if (is_repair) {
			repair_done = 1;
			zed_log_msg(LOG_INFO, RA_TAG"zpool_clear of pool '%s' "
			    "vdev %llu", zpool_get_name(zhp), vdev_guid);
			(void) zpool_vdev_clear(zhp, vdev_guid);
			zpool_close(zhp);
			continue;
		}

		/*
		 * Actively fault the device if needed.
		 */
		if (fault_device)
			(void) zpool_vdev_fault(zhp, vdev_guid, aux);
		if (degrade_device)
			(void) zpool_vdev_degrade(zhp, vdev_guid, aux);

		zed_log_msg(LOG_INFO, RA_TAG"zpool_vdev_%s: vdev %llu on '%s'",
		    fault_device ? "fault" : "degrade", vdev_guid,
		    zpool_get_name(zhp));

		/*
		 * Attempt to substitute a hot spare.
		 */
		replace_with_spare(zhp, vdev);
		zpool_close(zhp);
	}
#if 0
	if (strcmp(class, FM_LIST_REPAIRED_CLASS) == 0 && repair_done &&
	    nvlist_lookup_string(nvl, FM_SUSPECT_UUID, &uuid) == 0)
		fmd_case_uuresolved(hdl, uuid);
#endif
}

int
zfs_retire_init(libzfs_handle_t *zfs_hdl)
{
	if ((g_zrd.zrd_hdl = zfs_hdl) == NULL)
		return (-1);

	zed_log_msg(LOG_INFO, RA_TAG"zfs_retire_init");

	return (0);
}

void
zfs_retire_fini(void)
{
	zed_log_msg(LOG_INFO, RA_TAG"zfs_retire_fini");

	zfs_retire_clear_data(&g_zrd);
	g_zrd.zrd_hdl = NULL;
}
