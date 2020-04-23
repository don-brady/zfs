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

struct objset;
struct nvlist;
struct zvol_state;

typedef struct objset objset_t;
typedef struct nvlist nvlist_t;
typedef struct zvol_state zvol_state_t;
typedef int zprop_source_t;

int
zvol_init(void)
{
	return (0);
}

void
zvol_fini(void)
{
}

int
zvol_set_volsize(const char *name, uint64_t volsize)
{
	return (EROFS);
}

int
zvol_check_volblocksize(const char *name, uint64_t volblocksize)
{
	return (ENOTSUP);
}

int
zvol_get_stats(objset_t *os, nvlist_t *nv)
{
	return (ENOENT);
}

zvol_state_t *
zvol_suspend(const char *name)
{
	return (NULL);
}

int
zvol_set_snapdev(const char *ddname, zprop_source_t source, uint64_t snapdev)
{
	return (0);
}

int
zvol_check_volsize(uint64_t volsize, uint64_t blocksize)
{
	return (0);
}

int
zvol_resume(zvol_state_t *zv)
{
	return (0);
}

int
zvol_set_volmode(const char *ddname, zprop_source_t source, uint64_t volmode)
{
	return (0);
}
