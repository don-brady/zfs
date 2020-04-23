dnl #
dnl # Check for libfuse3 - required ZFS Fuse builds
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_LIBFUSE3], [
	LIBFUSE3=

	AC_CHECK_HEADER([fuse3/fuse_lowlevel.h], [
	    user_libfuse3=yes
	    AC_SUBST([LIBFUSE3], ["-lfuse3"])
	    AC_DEFINE([HAVE_LIBFUSE3], 1, [Define if you have libfuse3])
	], [
	    user_libfuse3=no
	])
])
