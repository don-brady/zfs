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
/*
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

// #include <fmd_alloc.h>
// #include <fmd_string.h>
// #include <fmd_subr.h>
// #include <fmd_api.h>
// #include <fmd_serd.h>
// #include <fmd.h>

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/list.h>
#include <sys/time.h>

#include "fmd_serd.h"
#include "../zed_log.h"

/*
 * one event per error report
 */
typedef struct fmd_serd_elem {
	list_node_t	se_list;	/* linked list forward/back pointers */
	hrtime_t	se_hrt;		/* upper bound on event hrtime */
} fmd_serd_elem_t;

typedef struct fmd_serd_eng {
	char		*sg_name;	/* string name for this engine */
	struct fmd_serd_eng *sg_next;	/* next engine on hash chain */
	list_t		sg_list;	/* list of fmd_serd_elem_t's */
	uint_t		sg_count;	/* count of events in sg_list */
	uint_t		sg_flags;	/* engine flags (see below) */
	uint_t		sg_n;		/* engine N parameter (event count) */
	hrtime_t	sg_t;		/* engine T parameter (nanoseconds) */
} fmd_serd_eng_t;

#define	FMD_SERD_FIRED	0x1		/* error rate has exceeded threshold */
#define	FMD_SERD_DIRTY	0x2		/* engine needs to be checkpointed */

typedef void fmd_serd_eng_f(fmd_serd_eng_t *, void *);

typedef struct fmd_serd_hash {
	fmd_serd_eng_t	**sh_hash;	/* hash bucket array for buffers */
	uint_t		sh_hashlen;	/* length of hash bucket array */
	uint_t		sh_count;	/* count of engines in hash */
} fmd_serd_hash_t;

#define	FMD_STR_BUCKETS		211

static void fmd_serd_hash_create(fmd_serd_hash_t *shp);
static void fmd_serd_hash_destroy(fmd_serd_hash_t *shp);

static fmd_serd_eng_t *fmd_serd_eng_lookup(fmd_serd_hash_t *shp,
    const char *name);
static fmd_serd_eng_t *fmd_serd_eng_insert(fmd_serd_hash_t *shp,
    const char *name, uint_t n, hrtime_t t);
static void fmd_serd_eng_delete(fmd_serd_hash_t *shp, const char *name);
static void fmd_serd_eng_reset(fmd_serd_eng_t *sgp);
static int fmd_serd_eng_record(fmd_serd_eng_t *sgp, hrtime_t hrt);
// static int fmd_serd_eng_contains(fmd_serd_eng_t *sgp, fmd_event_t *ep);

fmd_serd_hash_t mod_serds;	/* hash of serd engs (one per vdev) */

/*
 * SERD FMD Interface (frontend)
 */

void
fmd_serd_init(void)
{
	fmd_serd_hash_create(&mod_serds);
}

void
fmd_serd_fini(void)
{
	fmd_serd_hash_destroy(&mod_serds);
}

void
fmd_serd_create(const char *name, uint_t n, hrtime_t t)
{
	if (fmd_serd_eng_lookup(&mod_serds, name) != NULL) {
		zed_log_msg(LOG_ERR, "failed to create serd engine '%s': "
		    " name already exists", name);
		return;
	}

	(void) fmd_serd_eng_insert(&mod_serds, name, n, t);
}

void
fmd_serd_destroy(const char *name)
{
	fmd_serd_eng_delete(&mod_serds, name);
}

int
fmd_serd_exists(const char *name)
{
	int rv = (fmd_serd_eng_lookup(&mod_serds, name) != NULL);

	return (rv);
}

void
fmd_serd_reset(const char *name)
{
	fmd_serd_eng_t *sgp;

	if ((sgp = fmd_serd_eng_lookup(&mod_serds, name)) == NULL) {
		zed_log_msg(LOG_ERR, "serd engine '%s' does not exist", name);
		return;
	}

	fmd_serd_eng_reset(sgp);
}

int
fmd_serd_record(const char *name, uint64_t ena, const struct timespec *tsp)
{
	fmd_serd_eng_t *sgp;
	hrtime_t hrt;
	int err;

	if ((sgp = fmd_serd_eng_lookup(&mod_serds, name)) == NULL) {
		zed_log_msg(LOG_ERR, "failed to add record to serd engine '%s'",
		    name);
		return (FMD_B_FALSE);
	}

	/*
	 * Will need to normalized this if we persistently store the case data
	 */
	hrt = tsp->tv_sec * NANOSEC + tsp->tv_nsec;

	err = fmd_serd_eng_record(sgp, hrt);

//	if (sgp->sg_flags & FMD_SERD_DIRTY)
//		fmd_module_setdirty(mp);

	return (err);
}

/*
 * SERD Engine Backend
 */

/*
 * Compute the delta between events in nanoseconds.  To account for very old
 * events which are replayed, we must handle the case where time is negative.
 * We convert the hrtime_t's to unsigned 64-bit integers and then handle the
 * case where 'old' is greater than 'new' (i.e. high-res time has wrapped).
 */
hrtime_t
fmd_event_delta(hrtime_t t1, hrtime_t t2)
{
	uint64_t old = t1;
	uint64_t new = t2;

	return (new >= old ? new - old : (UINT64_MAX - old) + new + 1);
}

static fmd_serd_eng_t *
fmd_serd_eng_alloc(const char *name, uint64_t n, hrtime_t t)
{
	fmd_serd_eng_t *sgp;

	sgp = malloc(sizeof (fmd_serd_eng_t));
	bzero(sgp, sizeof (fmd_serd_eng_t));

	sgp->sg_name = strdup(name);
	sgp->sg_flags = FMD_SERD_DIRTY;
	sgp->sg_n = n;
	sgp->sg_t = t;

	list_create(&sgp->sg_list, sizeof (fmd_serd_elem_t),
	    offsetof(fmd_serd_elem_t, se_list));

	return (sgp);
}

static void
fmd_serd_eng_free(fmd_serd_eng_t *sgp)
{
	fmd_serd_eng_reset(sgp);
	free(sgp->sg_name);
	list_destroy(&sgp->sg_list);
	free(sgp);
}

static ulong_t
fmd_strhash(const char *key)
{
	ulong_t g, h = 0;
	const char *p;

	for (p = key; *p != '\0'; p++) {
		h = (h << 4) + *p;

		if ((g = (h & 0xf0000000)) != 0) {
			h ^= (g >> 24);
			h ^= g;
		}
	}

	return (h);
}

static void
fmd_serd_hash_create(fmd_serd_hash_t *shp)
{
	shp->sh_hashlen = FMD_STR_BUCKETS;
	shp->sh_hash = calloc(shp->sh_hashlen, sizeof (void *));
	shp->sh_count = 0;
}

static void
fmd_serd_hash_destroy(fmd_serd_hash_t *shp)
{
	fmd_serd_eng_t *sgp, *ngp;
	uint_t i;

	for (i = 0; i < shp->sh_hashlen; i++) {
		for (sgp = shp->sh_hash[i]; sgp != NULL; sgp = ngp) {
			ngp = sgp->sg_next;
			fmd_serd_eng_free(sgp);
		}
	}

	free(shp->sh_hash);
	bzero(shp, sizeof (fmd_serd_hash_t));
}

#if 0
static void
fmd_serd_hash_apply(fmd_serd_hash_t *shp, fmd_serd_eng_f *func, void *arg)
{
	fmd_serd_eng_t *sgp;
	uint_t i;

	for (i = 0; i < shp->sh_hashlen; i++) {
		for (sgp = shp->sh_hash[i]; sgp != NULL; sgp = sgp->sg_next)
			func(sgp, arg);
	}
}

static uint_t
fmd_serd_hash_count(fmd_serd_hash_t *shp)
{
	return (shp->sh_count);
}

static int
fmd_serd_hash_contains(fmd_serd_hash_t *shp, fmd_event_t *ep)
{
	fmd_serd_eng_t *sgp;
	uint_t i;

	for (i = 0; i < shp->sh_hashlen; i++) {
		for (sgp = shp->sh_hash[i]; sgp != NULL; sgp = sgp->sg_next) {
			if (fmd_serd_eng_contains(sgp, ep)) {
//				fmd_event_transition(ep, FMD_EVS_ACCEPTED);
				return (1);
			}
		}
	}

	return (0);
}
#endif

static fmd_serd_eng_t *
fmd_serd_eng_insert(fmd_serd_hash_t *shp,
    const char *name, uint_t n, hrtime_t t)
{
	uint_t h = fmd_strhash(name) % shp->sh_hashlen;
	fmd_serd_eng_t *sgp = fmd_serd_eng_alloc(name, n, t);

	zed_log_msg(LOG_INFO, "fmd_serd_eng_insert: %s N %d T %d",
	    name, (int)n, (int)t);

	sgp->sg_next = shp->sh_hash[h];
	shp->sh_hash[h] = sgp;
	shp->sh_count++;

	return (sgp);
}

static fmd_serd_eng_t *
fmd_serd_eng_lookup(fmd_serd_hash_t *shp, const char *name)
{
	uint_t h = fmd_strhash(name) % shp->sh_hashlen;
	fmd_serd_eng_t *sgp;

	for (sgp = shp->sh_hash[h]; sgp != NULL; sgp = sgp->sg_next) {
		if (strcmp(name, sgp->sg_name) == 0)
			return (sgp);
	}

	return (NULL);
}

static void
fmd_serd_eng_delete(fmd_serd_hash_t *shp, const char *name)
{
	uint_t h = fmd_strhash(name) % shp->sh_hashlen;
	fmd_serd_eng_t *sgp, **pp = &shp->sh_hash[h];

	zed_log_msg(LOG_INFO, "fmd_serd_eng_delete %s", name);

	for (sgp = *pp; sgp != NULL; sgp = sgp->sg_next) {
		if (strcmp(sgp->sg_name, name) != 0)
			pp = &sgp->sg_next;
		else
			break;
	}

	if (sgp != NULL) {
		*pp = sgp->sg_next;
		fmd_serd_eng_free(sgp);
		assert(shp->sh_count != 0);
		shp->sh_count--;
	}
}

static void
fmd_serd_eng_discard(fmd_serd_eng_t *sgp, fmd_serd_elem_t *sep)
{
	list_remove(&sgp->sg_list, sep);
	sgp->sg_count--;

	zed_log_msg(LOG_INFO, "fmd_serd_eng_discard: %s, %d remaining",
	    sgp->sg_name, (int)sgp->sg_count);

//	fmd_event_rele(sep->se_event);
	free(sep);
}

#if 0
static int
fmd_serd_eng_contains(fmd_serd_eng_t *sgp, fmd_event_t *ep)
{
	fmd_serd_elem_t *sep;

	for (sep = list_head(&sgp->sg_list);
	    sep != NULL; sep = list_next(&sgp->sg_list, sep)) {
		if (fmd_event_equal(sep->se_event, ep))
			return (1);
	}

	return (0);
}
#endif

static int
fmd_serd_eng_record(fmd_serd_eng_t *sgp, hrtime_t hrt)
{
	fmd_serd_elem_t *sep, *oep;

	/*
	 * If the fired flag is already set, return false and discard the
	 * event.  This means that the caller will only see the engine "fire"
	 * once until fmd_serd_eng_reset() is called.  The fmd_serd_eng_fired()
	 * function can also be used in combination with fmd_serd_eng_record().
	 */
	if (sgp->sg_flags & FMD_SERD_FIRED) {
#if 0
		zed_log_msg(LOG_INFO, "fmd_serd_eng_record: %s Already Fired!",
		    sgp->sg_name);
#endif
		return (FMD_B_FALSE);
	}

	while (sgp->sg_count >= sgp->sg_n)
		fmd_serd_eng_discard(sgp, list_tail(&sgp->sg_list));

//	fmd_event_hold(ep);
//	fmd_event_transition(ep, FMD_EVS_ACCEPTED);

	sep = malloc(sizeof (fmd_serd_elem_t));
	sep->se_hrt = hrt;

	list_insert_head(&sgp->sg_list, sep);
	sgp->sg_count++;

	zed_log_msg(LOG_INFO, "fmd_serd_eng_record: %s of %d (%llu)",
	    sgp->sg_name, (int)sgp->sg_count, (long long unsigned)hrt);

	/*
	 * Pick up the oldest element pointer for comparison to 'sep'.  We must
	 * do this after adding 'sep' because 'oep' and 'sep' can be the same.
	 */
	oep = list_tail(&sgp->sg_list);

	if (sgp->sg_count >= sgp->sg_n &&
	    fmd_event_delta(oep->se_hrt, sep->se_hrt) <= sgp->sg_t) {
		sgp->sg_flags |= FMD_SERD_FIRED | FMD_SERD_DIRTY;
		zed_log_msg(LOG_INFO, "SERD fired %s", sgp->sg_name);
		return (FMD_B_TRUE);
	}

	sgp->sg_flags |= FMD_SERD_DIRTY;
	return (FMD_B_FALSE);
}

#if 0
static int
fmd_serd_eng_fired(fmd_serd_eng_t *sgp)
{
	return (sgp->sg_flags & FMD_SERD_FIRED);
}

static int
fmd_serd_eng_empty(fmd_serd_eng_t *sgp)
{
	return (sgp->sg_count == 0);
}
#endif

static void
fmd_serd_eng_reset(fmd_serd_eng_t *sgp)
{
	zed_log_msg(LOG_INFO, "fmd_serd_eng_reset");

	while (sgp->sg_count != 0)
		fmd_serd_eng_discard(sgp, list_head(&sgp->sg_list));

	sgp->sg_flags &= ~FMD_SERD_FIRED;
	sgp->sg_flags |= FMD_SERD_DIRTY;
}

#if 0
static void
fmd_serd_eng_gc(fmd_serd_eng_t *sgp)
{
	fmd_serd_elem_t *sep, *nep;
	hrtime_t hrt;

	if (sgp->sg_count == 0 || (sgp->sg_flags & FMD_SERD_FIRED))
		return; /* no garbage collection needed if empty or fired */

	sep = list_head(&sgp->sg_list);
	hrt = fmd_event_hrtime(sep->se_event) - sgp->sg_t;

	for (sep = list_head(&sgp->sg_list); sep != NULL; sep = nep) {
		if (fmd_event_hrtime(sep->se_event) >= hrt)
			break; /* sep and subsequent events are all within T */

		nep = list_next(&sgp->sg_list, sep);
		fmd_serd_eng_discard(sgp, sep);
		sgp->sg_flags |= FMD_SERD_DIRTY;
	}
}

static void
fmd_serd_eng_commit(fmd_serd_eng_t *sgp)
{
	fmd_serd_elem_t *sep;

	if (!(sgp->sg_flags & FMD_SERD_DIRTY))
		return; /* engine has not changed since last commit */

	for (sep = list_head(&sgp->sg_list); sep != NULL;
	    sep = list_next(&sgp->sg_list, sep))
		fmd_event_commit(sep->se_event);

	sgp->sg_flags &= ~FMD_SERD_DIRTY;
}

static void
fmd_serd_eng_clrdirty(fmd_serd_eng_t *sgp)
{
	sgp->sg_flags &= ~FMD_SERD_DIRTY;
}
#endif
