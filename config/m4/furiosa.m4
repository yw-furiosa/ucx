#
# Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

AC_DEFUN([UCX_CHECK_FURIOSA],[

AS_IF([test "x$furiosa_checked" != "xyes"],
   [
    AC_ARG_WITH([furiosa],
                [AS_HELP_STRING([--with-furiosa=(DIR)],
                    [Enable the use of Furiosa NPU (default is guess).])],
                [], [with_furiosa=guess])

    AS_IF([test "x$with_furiosa" = "xno"],
        [
         furiosa_happy="no"
        ],
        [
         save_CPPFLAGS="$CPPFLAGS"
         save_LDFLAGS="$LDFLAGS"
         save_LIBS="$LIBS"

         FURIOSA_CPPFLAGS=""
         FURIOSA_LDFLAGS=""
         FURIOSA_LIBS=""

         AS_IF([test ! -z "$with_furiosa" -a "x$with_furiosa" != "xyes" -a "x$with_furiosa" != "xguess"],
               [FURIOSA_CPPFLAGS="-I$with_furiosa/include"
                FURIOSA_LDFLAGS="-L$with_furiosa/lib"])

         CPPFLAGS="$CPPFLAGS $FURIOSA_CPPFLAGS"
         LDFLAGS="$LDFLAGS $FURIOSA_LDFLAGS"

         dnl Furiosa NPU uses renegade_driver directly via ioctl.
         dnl Check for /dev/rngd directory existence or header availability.
         dnl For PoC, we only need standard system headers (ioctl, mmap).
         AC_CHECK_HEADERS([sys/ioctl.h],
                          [furiosa_happy="yes"], [furiosa_happy="no"])

         CPPFLAGS="$save_CPPFLAGS"
         LDFLAGS="$save_LDFLAGS"
         LIBS="$save_LIBS"

         AS_IF([test "x$furiosa_happy" = "xyes"],
               [AC_SUBST([FURIOSA_CPPFLAGS], ["$FURIOSA_CPPFLAGS"])
                AC_SUBST([FURIOSA_LDFLAGS], ["$FURIOSA_LDFLAGS"])
                AC_SUBST([FURIOSA_LIBS], ["$FURIOSA_LIBS"])
                AC_DEFINE([HAVE_FURIOSA], 1, [Enable Furiosa NPU support])],
               [AS_IF([test "x$with_furiosa" != "xguess"],
                      [AC_MSG_ERROR([Furiosa NPU support requested but required headers not found])],
                      [AC_MSG_WARN([Furiosa NPU support not available])])])

        ]) # "x$with_furiosa" = "xno"

        furiosa_checked=yes
        AM_CONDITIONAL([HAVE_FURIOSA], [test "x$furiosa_happy" != xno])

   ]) # "x$furiosa_checked" != "xyes"

]) # UCX_CHECK_FURIOSA
