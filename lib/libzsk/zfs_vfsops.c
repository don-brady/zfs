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

#include <sys/types.h>
#include <sys/param.h>
#include <errno.h>

typedef struct dsl_dataset dsl_dataset_t;
typedef struct zfsvfs zfsvfs_t;
typedef struct objset objset_t;
typedef int zfs_prop_t;

void
zfs_init(void)
{
}

void
zfs_fini(void)
{
}

int
zfsvfs_create(const char *osname, boolean_t readonly, zfsvfs_t **zfvp)
{
	return (EINVAL);
}

void
zfsvfs_free(zfsvfs_t *zfsvfs)
{
}

int
zfs_end_fs(zfsvfs_t *zfsvfs, dsl_dataset_t *ds)
{
	return (0);
}

int
zfs_suspend_fs(zfsvfs_t *zfsvfs)
{
	return (0);
}

int
zfs_resume_fs(zfsvfs_t *zfsvfs, dsl_dataset_t *ds)
{
	return (0);
}


int
zfs_set_version(zfsvfs_t *zfsvfs, uint64_t newvers)
{
	return (EINVAL);
}

int
zfs_get_zplprop(objset_t *os, zfs_prop_t prop, uint64_t *value)
{
	return (ENOENT);
}
