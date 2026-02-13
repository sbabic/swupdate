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

ifeq ($(CONFIG_PKCS11),y)
tests-$(CONFIG_ENCRYPTED_IMAGES) += test_crypt_pkcs11
endif
tests-$(CONFIG_ENCRYPTED_IMAGES) += test_crypt
tests-$(CONFIG_HASH_VERIFY) += test_hash
ifeq ($(CONFIG_SIGALG_RAWRSA),y)
tests-$(CONFIG_SIGNED_IMAGES) += test_verify
endif
tests-$(CONFIG_SURICATTA_HAWKBIT) += test_json
tests-$(CONFIG_SURICATTA_HAWKBIT) += test_server_hawkbit
tests-$(CONFIG_MONGOOSE) += test_mongoose_upload
tests-y += test_util
tests-$(CONFIG_CFI) += test_flash_handler

ccflags-y += -I$(src)/../

TARGETS    = $(addprefix $(obj)/, $(tests-y))
tests-objs = $(addsuffix .o,   $(TARGETS))
tests-lnk  = $(addsuffix .lnk, $(TARGETS))
targets   += $(addsuffix .o,   $(tests-y))

ifneq ($(CONFIG_EXTRA_LDFLAGS),)
EXTRA_LDFLAGS += $($(STRIP) $(subst ",,$(CONFIG_EXTRA_LDFLAGS)))#"))
endif
ifneq ($(CONFIG_EMBEDDED_SURICATTA_LUA),)
# Strip quotes from filename
CONFIG_EMBEDDED_SURICATTA_LUA_SOURCE := $(patsubst "%",%,$(CONFIG_EMBEDDED_SURICATTA_LUA_SOURCE))
ifeq ($(findstring /,$(word 1,$(subst /, /,$(CONFIG_EMBEDDED_SURICATTA_LUA_SOURCE)))),)
# It's not an absolute path, assume it's relative to $(srcdir)
ifeq ($(realpath $(srctree)/$(CONFIG_EMBEDDED_SURICATTA_LUA_SOURCE)),)
$(error File specified in CONFIG_EMBEDDED_SURICATTA_LUA_SOURCE=$(CONFIG_EMBEDDED_SURICATTA_LUA_SOURCE) not found)
endif
CONFIG_EMBEDDED_SURICATTA_LUA_SOURCE := $(realpath $(srctree)/$(CONFIG_EMBEDDED_SURICATTA_LUA_SOURCE))
endif
EXTRA_LDFLAGS += -Wl,--format=binary -Wl,$(CONFIG_EMBEDDED_SURICATTA_LUA_SOURCE) -Wl,--format=default
endif

## transfer __wrap_*() functions to according linker flags via rudimentary regex awk parsing,
## e.g. __wrap_channel_get becomes -Wl,--wrap=channel_get.
LDMOCKS = `/usr/bin/env gawk '/^[^(extern)|(static)]*([\*_[:alnum:]]+[[:blank:]]+)+[\*]*__wrap_/{where = match($$0,"__wrap_[_[:alnum:]]+"); if (RSTART > 0) { print "-Wl,--wrap=" substr($$0, RSTART+7, RLENGTH-7);}}' $(basename $(srctree)/$@).c`

quiet_cmd_linktestexe = LD      $(basename $@)
      cmd_linktestexe = $(srctree)/scripts/trylink \
						"$(basename $@)" \
						"$(CC)" \
						"$(KBUILD_CFLAGS)" \
						"$(LDFLAGS) $(EXTRA_LDFLAGS) $(LDMOCKS)" \
						"$(basename $@).o $(subst core/built-in.o,core/built-in.o.tmp,$(SWOBJS))" \
						"$(SWLIBS)" \
						"$(LDLIBS) cmocka"

EXECUTE_TEST = echo "RUN $(subst $(obj)/,,$(var))"; LD_LIBRARY_PATH=$(objtree) CMOCKA_MESSAGE_OUTPUT=TAP SOFTHSM2_CONF=$(DATADIR)/token/softhsm.conf $(var)

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

$(objtree)/core/built-in.o.tmp: $(objtree)/core/built-in.o
	$(Q)$(STRIP) -N main -o $(objtree)/core/built-in.o.tmp $(objtree)/core/built-in.o

$(obj)/%.lnk: $(obj)/%.o $(objtree)/core/built-in.o.tmp
	$(Q)$(call cmd,linktestexe)

DATADIR := test/data

PHONY += PREPARE_DATA
PREPARE_DATA:
	$(Q)[ "$(srctree)" -ef "$(objtree)" ] || cp -r $(srctree)/test/data $(objtree)/test/data

$(obj)/test_verify.o: PREPARE_DATA $(DATADIR)/signature $(DATADIR)/signing-pubkey.pem

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

ifeq ($(CONFIG_PKCS11),y)
$(obj)/test_crypt_pkcs11.o: $(DATADIR)/softshm

TOKEN_AES_KEY := dd020ce5ebd5c468556288d6a75169c88a5b335d9f569e30751c50401467d230
TOKEN_AES_IV := c1f390d21dd06118cbd333144a3318ca

.INTERMEDIATE: $(DATADIR)/softshm
$(DATADIR)/softshm: export SOFTHSM2_CONF=$(DATADIR)/token/softhsm.conf
$(DATADIR)/softshm: $(DATADIR)/token/softhsm.conf PREPARE_DATA
	$(if $(Q),@echo "  INIT     $@")
	$(Q)rm -rf $(DATADIR)/token/*/

	$(if $(Q),@echo "  GEN      $(DATADIR)/token/original.data")
	$(Q)openssl rand 131075 > $(DATADIR)/token/original.data

	$(if $(Q),@echo "  ENCRYPT  $(DATADIR)/token/original.data")
	$(Q)openssl enc -aes-256-cbc -in $(DATADIR)/token/original.data -out $(DATADIR)/token/encrypted.data -K $(TOKEN_AES_KEY) -iv $(TOKEN_AES_IV)
	$(Q)echo -n "$(TOKEN_AES_IV)" | xxd -p -r > $(DATADIR)/token/encrypted.data.iv

	$(if $(Q),@echo "  IMPORT   $(DATADIR)/token/aes.key")
	$(Q)echo -n "$(TOKEN_AES_KEY)" | xxd -p -r > $(DATADIR)/token/aes.key
	$(Q)softhsm2-util --init-token --slot 0 --label "TestToken" --so-pin 123456 --pin 1234
	$(Q)softhsm2-util --import $(DATADIR)/token/aes.key --aes --token "TestToken" --label "AES key" --id A1B2 --pin 1234
endif

.PHONY: $(PHONY)
