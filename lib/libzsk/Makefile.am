include $(top_srcdir)/config/Rules.am

VPATH = \
	$(top_srcdir)/module/zfs \
	$(top_srcdir)/module/zcommon \
	$(top_srcdir)/module/os/linux/zfs \
	$(top_srcdir)/lib/libzsk

# Suppress unused but set variable warnings often due to ASSERTs
AM_CFLAGS += $(NO_UNUSED_BUT_SET_VARIABLE)

# Includes kernel code generate warnings for large stack frames
AM_CFLAGS += $(FRAME_LARGER_THAN)

AM_CFLAGS += -DLIB_ZPOOL_BUILD

# AM_CFLAGS += -I$(top_srcdir)/include/os/linux/zfs
AM_CFLAGS += -I$(top_srcdir)/lib/libzsk/include

lib_LTLIBRARIES = libzsk.la

USER_C = \
	zvol_os.c \
	zfs_vfsops.c \
	zfs_znode.c \
	zfs_ioctl_os.c

KERNEL_C = \
	fm.c \
	zfs_ioctl.c \
	zfs_fuid.c \
	zfs_quota.c

nodist_libzsk_la_SOURCES = \
	$(USER_C) \
	$(KERNEL_C)

libzsk_la_LIBADD = \
	$(top_builddir)/lib/libnvpair/libnvpair.la \
	$(top_builddir)/lib/libzutil/libzutil.la

libzsk_la_LIBADD += $(ZLIB) -ldl
libzsk_la_LDFLAGS = -pthread -version-info 2:0:0

EXTRA_DIST = $(USER_C)
