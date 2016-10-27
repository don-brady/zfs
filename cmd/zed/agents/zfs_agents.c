/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License Version 1.0 (CDDL-1.0).
 * You can obtain a copy of the license from the top-level file
 * "OPENSOLARIS.LICENSE" or at <http://opensource.org/licenses/CDDL-1.0>.
 * You may not use this file except in compliance with the license.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2016, Intel Corporation.
 */

#include <libnvpair.h>
#include <libzfs.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/list.h>
#include <sys/time.h>
#include <sys/sysevent/eventdefs.h>
#include <sys/sysevent/dev.h>
#include <sys/fm/protocol.h>
#include <sys/fm/fs/zfs.h>
#include <pthread.h>
#include <unistd.h>

#include "zfs_agents.h"
#include "fmd_api.h"
#include "../zed_log.h"

/*
 * agent dispatch code
 */

static pthread_mutex_t	agent_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	agent_cond = PTHREAD_COND_INITIALIZER;
static list_t		agent_events;	/* list of pending events */
static int		agent_exiting;

typedef struct agent_event {
	char		ae_class[64];
	char		ae_subclass[32];
	nvlist_t	*ae_nvl;
	list_node_t	ae_node;
} agent_event_t;

pthread_t g_agents_tid;

libzfs_handle_t *g_zfs_hdl;

/* guid search data */
typedef struct guid_search {
	uint64_t	gs_pool_guid;
	uint64_t	gs_vdev_guid;
	char		*gs_devid;
} guid_search_t;

static void
zfs_agent_iter_vdev(zpool_handle_t *zhp, nvlist_t *nvl, void *arg)
{
	guid_search_t *gsp = arg;
	char *path = NULL;
	uint_t c, children;
	nvlist_t **child;

	/*
	 * First iterate over any children.
	 */
	if (nvlist_lookup_nvlist_array(nvl, ZPOOL_CONFIG_CHILDREN,
	    &child, &children) == 0) {
		for (c = 0; c < children; c++)
			zfs_agent_iter_vdev(zhp, child[c], gsp);
		return;
	}
	/*
	 * On a devid match, grab the vdev guid
	 */
	if ((gsp->gs_vdev_guid == 0) &&
	    (nvlist_lookup_string(nvl, ZPOOL_CONFIG_DEVID, &path) == 0) &&
	    (strcmp(gsp->gs_devid, path) == 0)) {
		(void) nvlist_lookup_uint64(nvl, ZPOOL_CONFIG_GUID,
		    &gsp->gs_vdev_guid);
	}
}

static int
zfs_agent_iter_pool(zpool_handle_t *zhp, void *arg)
{
	guid_search_t *gsp = arg;
	nvlist_t *config, *nvl;

	/*
	 * For each vdev in this pool, look for a match by devid
	 */
	if ((config = zpool_get_config(zhp, NULL)) != NULL) {
		if (nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE,
		    &nvl) == 0) {
			zfs_agent_iter_vdev(zhp, nvl, gsp);
		}
	}
	/*
	 * if a match was found then grab the pool guid
	 */
	if (gsp->gs_vdev_guid) {
		(void) nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID,
		    &gsp->gs_pool_guid);
	}

	zpool_close(zhp);
	return (gsp->gs_vdev_guid != 0);
}

void
zfs_agent_post_event(const char *class, const char *subclass, nvlist_t *nvl)
{
	agent_event_t *event;

	if (subclass == NULL)
		subclass = "";

	event = malloc(sizeof (agent_event_t));
	if (event == NULL || nvlist_dup(nvl, &event->ae_nvl, 0) != 0) {
		if (event)
			free(event);
		return;
	}

	if (strcmp(class, "sysevent.fs.zfs.vdev_check") == 0) {
		class = EC_ZFS;
		subclass = ESC_ZFS_VDEV_CHECK;
	}

	/*
	 * On ZFS on Linux, we don't get the expected FM_RESOURCE_REMOVED
	 * ereport from vdev_disk layer after a hot unplug. Fortunately we
	 * get a EC_DEV_REMOVE from our disk monitor and it is a suitable
	 * proxy so we remap it here for the benefit of the diagnosis engine.
	 */
	if ((strcmp(class, EC_DEV_REMOVE) == 0) &&
	    (strcmp(subclass, ESC_DISK) == 0) &&
	    (nvlist_exists(nvl, ZFS_EV_VDEV_GUID) ||
	    nvlist_exists(nvl, DEV_IDENTIFIER))) {
		nvlist_t *payload = event->ae_nvl;
		struct timeval tv;
		int64_t tod[2];
		uint64_t pool_guid = 0, vdev_guid = 0;

		class = "resource.fs.zfs.removed";
		subclass = "";

		(void) nvlist_add_string(payload, FM_CLASS, class);
		(void) nvlist_lookup_uint64(nvl, ZFS_EV_POOL_GUID, &pool_guid);
		(void) nvlist_lookup_uint64(nvl, ZFS_EV_VDEV_GUID, &vdev_guid);

		/*
		 * For multipath, ZFS_EV_VDEV_GUID is missing so find it.
		 */
		if (vdev_guid == 0) {
			guid_search_t search = { 0 };

			(void) nvlist_lookup_string(nvl, DEV_IDENTIFIER,
			    &search.gs_devid);

			(void) zpool_iter(g_zfs_hdl, zfs_agent_iter_pool,
			    &search);
			pool_guid = search.gs_pool_guid;
			vdev_guid = search.gs_vdev_guid;
		}

		(void) nvlist_add_uint64(payload,
		    FM_EREPORT_PAYLOAD_ZFS_POOL_GUID, pool_guid);
		(void) nvlist_add_uint64(payload,
		    FM_EREPORT_PAYLOAD_ZFS_VDEV_GUID, vdev_guid);

		(void) gettimeofday(&tv, NULL);
		tod[0] = tv.tv_sec;
		tod[1] = tv.tv_usec;
		(void) nvlist_add_int64_array(payload, FM_EREPORT_TIME, tod, 2);

		zed_log_msg(LOG_INFO, "agent post event: mapping '%s' to '%s'",
		    EC_DEV_REMOVE, class);
	}

	(void) strlcpy(event->ae_class, class, sizeof (event->ae_class));
	(void) strlcpy(event->ae_subclass, subclass,
	    sizeof (event->ae_subclass));

	(void) pthread_mutex_lock(&agent_lock);
	list_insert_tail(&agent_events, event);
	(void) pthread_mutex_unlock(&agent_lock);

	(void) pthread_cond_signal(&agent_cond);
}

static void
zfs_agent_dispatch(const char *class, const char *subclass, nvlist_t *nvl)
{
	/*
	 * The diagnosis engine subscribes to the following events.
	 * On illumos these subscriptions reside in:
	 * 	/usr/lib/fm/fmd/plugins/zfs-diagnosis.conf
	 */
	if (strstr(class, "ereport.fs.zfs.") != NULL ||
	    strstr(class, "resource.fs.zfs.") != NULL ||
	    strcmp(class, "sysevent.fs.zfs.vdev_remove") == 0 ||
	    strcmp(class, "sysevent.fs.zfs.vdev_remove_dev") == 0 ||
	    strcmp(class, "sysevent.fs.zfs.pool_destroy") == 0) {
		fmd_module_recv(fmd_module_hdl("zfs-diagnosis"), nvl, class);
	}

	/*
	 * The retire agent subscribes to the following events.
	 * On illumos these subscriptions reside in:
	 * 	/usr/lib/fm/fmd/plugins/zfs-retire.conf
	 *
	 * NOTE: faults events come directy from our diagnosis engine
	 * and will not pass through the zfs kernel module.
	 */
	if (strcmp(class, FM_LIST_SUSPECT_CLASS) == 0 ||
	    strcmp(class, "resource.fs.zfs.removed") == 0 ||
	    strcmp(class, "resource.fs.zfs.statechange") == 0 ||
	    strcmp(class, "sysevent.fs.zfs.vdev_remove")  == 0) {
		fmd_module_recv(fmd_module_hdl("zfs-retire"), nvl, class);
	}

	/*
	 * The SLM module only consumes disk events and vdev check events
	 *
	 * NOTE: disk events come directly from disk monitor and will
	 * not pass through the zfs kernel module.
	 */
	if (strstr(class, "EC_dev_") != NULL ||
	    strcmp(class, EC_ZFS) == 0) {
		(void) zfs_slm_event(class, subclass, nvl);
	}
}

/*
 * Events are consumed and dispatched from this thread
 * An agent can also post an event so event list lock
 * is not held when calling an agent.
 * One event is consumed at a time.
 */
static void *
zfs_agent_consumer_thread(void *arg)
{
	for (;;) {
		agent_event_t *event;

		(void) pthread_mutex_lock(&agent_lock);

		/* wait for an event to show up */
		while (!agent_exiting && list_is_empty(&agent_events))
			(void) pthread_cond_wait(&agent_cond, &agent_lock);

		if (agent_exiting) {
			(void) pthread_mutex_unlock(&agent_lock);
			zed_log_msg(LOG_INFO, "zfs_agent_consumer_thread: "
			    "exiting");
			return (NULL);
		}

		if ((event = (list_head(&agent_events))) != NULL) {
			list_remove(&agent_events, event);

			(void) pthread_mutex_unlock(&agent_lock);

			/* dispatch to all event subscribers */
			zfs_agent_dispatch(event->ae_class, event->ae_subclass,
			    event->ae_nvl);

			nvlist_free(event->ae_nvl);
			free(event);
			continue;
		}

		(void) pthread_mutex_unlock(&agent_lock);
	}

	return (NULL);
}

void
zfs_agent_init(libzfs_handle_t *zfs_hdl)
{
	fmd_hdl_t *hdl;

	g_zfs_hdl = zfs_hdl;

	if (zfs_slm_init() != 0)
		zed_log_die("Failed to initialize zfs slm");
	zed_log_msg(LOG_INFO, "Add Agent: init");

	hdl = fmd_module_hdl("zfs-diagnosis");
	_zfs_diagnosis_init(hdl);
	if (!fmd_module_initialized(hdl))
		zed_log_die("Failed to initialize zfs diagnosis");

	hdl = fmd_module_hdl("zfs-retire");
	_zfs_retire_init(hdl);
	if (!fmd_module_initialized(hdl))
		zed_log_die("Failed to initialize zfs retire");

	list_create(&agent_events, sizeof (agent_event_t),
	    offsetof(struct agent_event, ae_node));

	if (pthread_create(&g_agents_tid, NULL, zfs_agent_consumer_thread,
	    NULL) != 0) {
		list_destroy(&agent_events);
		zed_log_die("Failed to initialize agents");
	}
}

void
zfs_agent_fini(void)
{
	fmd_hdl_t *hdl;
	agent_event_t *event;

	agent_exiting = 1;
	(void) pthread_cond_signal(&agent_cond);

	/* wait for zfs_enum_pools thread to complete */
	(void) pthread_join(g_agents_tid, NULL);

	/* drain any pending events */
	while ((event = (list_head(&agent_events))) != NULL) {
		list_remove(&agent_events, event);
		nvlist_free(event->ae_nvl);
		free(event);
	}

	list_destroy(&agent_events);

	if ((hdl = fmd_module_hdl("zfs-retire")) != NULL) {
		_zfs_retire_fini(hdl);
		fmd_hdl_unregister(hdl);
	}
	if ((hdl = fmd_module_hdl("zfs-diagnosis")) != NULL) {
		_zfs_diagnosis_fini(hdl);
		fmd_hdl_unregister(hdl);
	}

	zed_log_msg(LOG_INFO, "Add Agent: fini");
	zfs_slm_fini();

	g_zfs_hdl = NULL;
}

/*
 * In ZED context, all the FMA agents run in the same thread
 * and do not require a unique libzfs instance. Modules should
 * use these stubs.
 */
libzfs_handle_t *
__libzfs_init(void)
{
	return (g_zfs_hdl);
}

void
__libzfs_fini(libzfs_handle_t *hdl)
{
}
