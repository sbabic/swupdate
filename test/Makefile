# SPDX-License-Identifier: GPL-2.0-or-later

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

ifneq ($(CONFIG_PKCS11),y)
tests-$(CONFIG_ENCRYPTED_IMAGES) += test_crypt
endif
tests-$(CONFIG_HASH_VERIFY) += test_hash
ifeq ($(CONFIG_SIGALG_RAWRSA),y)
tests-$(CONFIG_SIGNED_IMAGES) += test_verify
endif
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
LDMOCKS = `/usr/bin/gawk '/^[^(extern)|(static)]*([\*_[:alnum:]]+[[:blank:]]+)+[\*]*__wrap_/{where = match($$0,"__wrap_[_[:alnum:]]+"); if (RSTART > 0) { print "-Wl,--wrap=" substr($$0, RSTART+7, RLENGTH-7);}}' $(basename $@).c`

quiet_cmd_linktestexe = LD      $(basename $@)
      cmd_linktestexe = $(srctree)/scripts/trylink \
						"$(basename $@)" \
						"$(CC)" \
						"$(KBUILD_CFLAGS)" \
						"$(LDFLAGS) $(EXTRA_LDFLAGS) $(LDMOCKS)" \
						"$(basename $@).o $(subst core/built-in.o,core/built-in.o.tmp,$(SWOBJS))" \
						"$(SWLIBS)" \
						"$(LDLIBS) cmocka"

EXECUTE_TEST = echo "RUN $(subst $(obj)/,,$(var))"; LD_LIBRARY_PATH=$(objtree) CMOCKA_MESSAGE_OUTPUT=TAP $(var)

PHONY += default
default:
	$(info please run 'make test' in swupdate main directory)

PHONY += tests
ifneq "$(tests-y)" ""
tests: $(tests-objs) $(tests-lnk)
	@+$(foreach var,$(TARGETS),$(EXECUTE_TEST);)
else
tests:
	@$(info nothing to test.)
	@:
endif

$(obj)/%.lnk: $(obj)/%.o $(objtree)/core/built-in.o
	$(Q)strip -N main -o $(objtree)/core/built-in.o.tmp $(objtree)/core/built-in.o
	$(Q)$(call cmd,linktestexe)

DATADIR := test/data

$(obj)/test_verify.o: $(DATADIR)/signature $(DATADIR)/signing-pubkey.pem

.INTERMEDIATE: $(DATADIR)/signature
$(DATADIR)/signature: $(DATADIR)/to-be-signed $(DATADIR)/signing-secret.pem
	$(if $(Q),@echo "  SIGN    $@")
	$(Q)openssl dgst -sha256 -sign $(DATADIR)/signing-secret.pem $(DATADIR)/to-be-signed > $@

.INTERMEDIATE: $(DATADIR)/signing-pubkey.pem
$(DATADIR)/signing-pubkey.pem: $(DATADIR)/signing-secret.pem
	$(if $(Q),@echo "  EXPORT  $@")
	$(Q)openssl rsa -in $< -out $@ -outform PEM -pubout 2>/dev/null

.INTERMEDIATE: $(DATADIR)/signing-secret.pem
$(DATADIR)/signing-secret.pem:
	$(if $(Q),@echo "  GEN     $@")
	$(Q)openssl genrsa -out $@ 2>/dev/null

.PHONY: $(PHONY)
