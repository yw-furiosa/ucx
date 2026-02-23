#
# Copyright (C) FuriosaAI, 2025-2026. ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

UCX_CHECK_FURIOSA

AS_IF([test "x$furiosa_happy" = "xyes"], [uct_modules="${uct_modules}:furiosa"])

AC_CONFIG_FILES([src/uct/furiosa/Makefile
                 src/uct/furiosa/ucx-furiosa.pc])
