include $(top_srcdir)/config/Rules.am

sbin_PROGRAMS = zsk

zsk_SOURCES = \
	zsk.c

zsk_LDADD = \
	$(top_builddir)/lib/libnvpair/libnvpair.la \
	$(top_builddir)/lib/libzpool/libzpool.la \
	$(top_builddir)/lib/libzsk/libzsk.la \
	-lfuse3

zsk_LDFLAGS = -pthread
