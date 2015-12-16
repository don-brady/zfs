dnl #
dnl # Check for libudev
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_LIBUDEV], [
	LIBUDEV=

	AC_CHECK_HEADER([libudev.h], [], [AC_MSG_FAILURE([
	*** libudev.h missing, libudev-devel package required])])

	AC_CHECK_LIB([udev], [udev_device_get_devlinks_list_entry], [], [AC_MSG_FAILURE([
	*** udev_device_get_devlinks_list_entry() missing, libudev-devel package required])])

	AC_CHECK_LIB([udev], [udev_device_new_from_subsystem_sysname], [], [AC_MSG_FAILURE([
	*** udev_device_new_from_subsystem_sysname() missing, libudev-devel package required])])

	AC_CHECK_LIB([udev], [udev_device_get_property_value], [], [AC_MSG_FAILURE([
	*** udev_device_get_property_value() missing, libudev-devel package required])])

	AC_SUBST([LIBUDEV], ["-ludev"])
	AC_DEFINE([HAVE_LIBUDEV], 1, [Define if you have libudev])
])
