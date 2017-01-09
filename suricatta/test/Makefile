## Author: Christian Storm
## Copyright (C) 2017, Siemens AG
##
## This program is free software; you can redistribute it and/or
## modify it under the terms of the GNU General Public License as
## published by the Free Software Foundation; either version 2 of
## the License, or (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program; if not, write to the Free Software
## Foundation, Inc.

tests-$(CONFIG_SURICATTA_HAWKBIT) += test_json
tests-$(CONFIG_SURICATTA_HAWKBIT) += test_server_hawkbit

ccflags-y += -I$(src)/../

TARGETS    = $(addprefix $(obj)/, $(tests-y))
tests-objs = $(addsuffix .o,   $(TARGETS))
tests-lnk  = $(addsuffix .lnk, $(TARGETS))
targets   += $(addsuffix .o,   $(tests-y))

ifneq ($(CONFIG_EXTRA_LDFLAGS),)
EXTRA_LDFLAGS += $(strip $(subst ",,$(CONFIG_EXTRA_LDFLAGS)))#"))
endif

## transfer __mock_*() functions to according linker flags via rudimentary regex awk parsing
LDMOCKS = `/usr/bin/awk '/^[^(extern)|(static)]*([\*_[:alnum:]]+[[:blank:]]+)+[\*]*__wrap_/{where = match($$0,"__wrap_[_[:alnum:]]+"); if (RSTART > 0) { print "-Wl,--wrap=" substr($$0, RSTART+7, RLENGTH-7);}}' $(basename $@).c`

quiet_cmd_linktestexe = LD      $(basename $@)
      cmd_linktestexe = $(srctree)/scripts/trylink \
						"$(basename $@)" \
						"$(CC)" \
						"$(KBUILD_CFLAGS)" \
						"$(LDFLAGS) $(EXTRA_LDFLAGS) $(LDMOCKS)" \
						"$(basename $@).o $(subst core/built-in.o,core/built-in.o.tmp,$(SWOBJS))" \
						"$(SWLIBS)" \
						"$(LDLIBS) cmocka"

EXECUTE_TEST = echo "RUN $(subst $(obj)/,,$(var))"; CMOCKA_MESSAGE_OUTPUT=TAP $(var)

PHONY += default
default:
	$(info please run 'make suricatta-tests' in swupdate main directory)

PHONY += tests
ifneq "$(tests-y)" ""
tests: $(tests-objs) $(tests-lnk)
	@+$(foreach var,$(TARGETS),$(EXECUTE_TEST);)
else
tests:
	@$(info suricatta is disabled, nothing to test.)
	@:
endif

$(obj)/%.lnk: $(objtree)/core/built-in.o
	$(Q)strip -N main -o $(objtree)/core/built-in.o.tmp $(objtree)/core/built-in.o
	$(Q)$(call cmd,linktestexe)

.PHONY: $(PHONY)
