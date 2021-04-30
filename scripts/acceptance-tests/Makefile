# (C) Copyright 2016
# Denis Osterland, Diehl Connectivity Solutions GmbH, Denis.Osterland@diehl.com.
#
# SPDX-License-Identifier:     GPL-2.0-or-later

.NOTPARALLEL:

SWU_CLEAN = rm -f /tmp/sw-description

include $(srctree)/scripts/acceptance-tests/CheckImage.mk

PHONY += tests
tests: $(tests-y)
	$(Q)echo "tests passed"
	$(Q)$(SWU_CLEAN)
