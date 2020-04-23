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
#include <string.h>

struct task_struct;
typedef struct zfsvfs zfsvfs_t;

void
zfs_ioctl_init_os(void)
{
}

boolean_t
zfs_vfs_held(zfsvfs_t *zfsvfs)
{
	return (0);
}

int
zfs_vfs_ref(zfsvfs_t **zfvp)
{
	return (0);
}

void
zfs_vfs_rele(zfsvfs_t *zfsvfs)
{
}

int
zfsdev_attach(void)
{
	return (0);
}

void
zfsdev_detach(void)
{
}

boolean_t
zfs_proc_is_caller(struct task_struct *t)
{
	return (0);
}

int
ddi_copyin(const void *from, void *to, size_t len, int flags)
{
//	printf("ddi_copyin %d bytes from %p to %p\n", (int)len, from, to);
	memcpy(to, from, len);
	return (0);
}

int
ddi_copyout(const void *from, void *to, size_t len, int flags)
{
//	printf("ddi_copyout %d bytes from %p to %p\n", (int)len, from, to);
	memcpy(to, from, len);
	return (0);
}

void xcopyout(void) {}
void is_system_labeled(void) {}
zfs_file_private(void) {}
secpolicy_zinject(void) {}
zfsctl_snapshot_unmount(void) {}
secpolicy_sys_config(void) {}
copyinstr(void) {}
groupmember(void) {}

