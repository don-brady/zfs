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
#include <sys/sysevent/eventdefs.h>
#include <sys/sysevent/dev.h>
#include <sys/fm/protocol.h>
#include <pthread.h>
#include <unistd.h>
#include "zfs_agents.h"
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

//	zed_log_msg(LOG_INFO, ">>> zfs_agent_post_event: %s", class);

	if (strcmp(class, "sysevent.fs.zfs.vdev_check") == 0) {
		class = "EC_zfs";
		subclass = "ESC_ZFS_vdev_check";
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
//	zed_log_msg(LOG_INFO, "<<< zfs_agent_dispatch: %s", class);

	/*
	 * The diagnosis engine subscribes to the following wild card events.
	 *
	 * NOTE: disk events come directly from disk monitor and will
	 * not pass through the zfs kernel module or this code path.
	 */
	if (strstr(class, "ereport.fs.zfs.") != NULL ||
	    strstr(class, "resource.fs.zfs.") != NULL) {
		zfs_diagnosis_recv(nvl, class);
	}

	/*
	 * The retire agent subscribes to the following resource events.
	 *
	 * NOTE: faults events come directy from diagnosis engine
	 * and will not pass through the zfs kernel module.
	 */
	if (strcmp(class, FM_LIST_SUSPECT_CLASS) == 0 ||
	    strcmp(class, "resource.fs.zfs.removed") == 0 ||
	    strcmp(class, "resource.fs.zfs.statechange") == 0 ||
	    strcmp(class,
	    "resource.sysevent.EC_zfs.ESC_ZFS_vdev_remove")  == 0) {
		zfs_retire_recv(nvl, class);
	}

	/*
	 * The SLM module only consumes vdev check events
	 *
	 * NOTE: disk events come directly from disk monitor and will
	 * not pass through the zfs kernel module.
	 */
	if (strstr(class, "EC_dev_") != NULL ||
	    strcmp(class, "EC_zfs") == 0) {
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
	if (zfs_slm_init(zfs_hdl) != 0)
		zed_log_die("Failed to initialize zfs slm");
	if (zfs_diagnosis_init(zfs_hdl) != 0)
		zed_log_die("Failed to initialize zfs diagnosis");
	if (zfs_retire_init(zfs_hdl) != 0)
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
	agent_event_t *event;

	zed_log_msg(LOG_INFO, "zfs_agent_fini");

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

	zfs_retire_fini();
	zfs_diagnosis_fini();
	zfs_slm_fini();
}
