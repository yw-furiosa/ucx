#
# Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

if HAVE_FURIOSA

SUBDIRS = .

module_LTLIBRARIES       = libuct_furiosa.la
libuct_furiosa_la_CPPFLAGS = $(BASE_CPPFLAGS) $(FURIOSA_CPPFLAGS)
libuct_furiosa_la_CFLAGS   = $(BASE_CFLAGS) $(LT_CFLAGS)
libuct_furiosa_la_LIBADD   = $(top_builddir)/src/ucs/libucs.la \
                              $(top_builddir)/src/uct/libuct.la
libuct_furiosa_la_LDFLAGS  = $(FURIOSA_LDFLAGS) $(FURIOSA_LIBS) -version-info $(SOVERSION)

noinst_HEADERS = \
    base/furiosa_base.h \
    furiosa_iface.h \
    furiosa_md.h

libuct_furiosa_la_SOURCES = \
    base/furiosa_base.c \
    furiosa_iface.c \
    furiosa_md.c

PKG_CONFIG_NAME=furiosa

include $(top_srcdir)/config/module.am

endif
