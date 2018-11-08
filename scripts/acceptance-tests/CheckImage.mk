# (C) Copyright 2016
# Denis Osterland, Diehl Connectivity Solutions GmbH, Denis.Osterland@diehl.com.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 2 of
# the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.   See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc.

#
# test commands for --check command-line option
#
SWU_CHECK_BASE = ./swupdate -l 5 -c $(if $(CONFIG_SIGNED_IMAGES),-k $(obj)/cacert.pem) $(if $(strip $(filter %.cfg, $^)), -f $(filter %.cfg, $^))
SWU_CHECK = $(SWU_CHECK_BASE) $(if $(CONFIG_HW_COMPATIBILITY),-H test:1) $(if $(strip $(filter-out FORCE,$<)),-i $<) $(if $(strip $(KBUILD_VERBOSE:0=)),,>/dev/null 2>&1)

quiet_cmd_swu_check_assert_false = RUN     $@
      cmd_swu_check_assert_false = $(SWU_CLEAN); if $(SWU_CHECK); then false; fi

quiet_cmd_swu_check_assert_true = RUN     $@
      cmd_swu_check_assert_true = $(SWU_CLEAN); $(SWU_CHECK)

quiet_cmd_swu_check_inv_websrv = RUN     $@
      cmd_swu_check_inv_websrv = $(SWU_CLEAN); if $(SWU_CHECK_BASE) -w "-document_root $(srctree)" >/dev/null 2>&1; then false; fi

quiet_cmd_swu_check_inv_suricatta = RUN     $@
      cmd_swu_check_inv_suricatta = $(SWU_CLEAN); if $(SWU_CHECK_BASE) -u "-t default -i 42 -u localhost:8080" >/dev/null 2>&1; then false; fi

quiet_cmd_mkswu = MKSWU   $@
      cmd_mkswu = mkdir -p $(dir $@); cd $(dir $<); for l in $(patsubst $(dir $<)%,%,$(filter-out FORCE,$^)); do echo "$$l"; done | cpio -ov -H crc > $(objtree)/$@

quiet_cmd_sign_desc = SIGN    $@
      cmd_sign_desc = openssl cms -sign -in $< -out $@ -signer $(obj)/signer.pem -outform DER -nosmimecap -binary

URL = https://raw.githubusercontent.com/openssl/openssl/master/demos/cms
quiet_cmd_download = GET     $@
      cmd_download = rm -f $@.tmp && wget -O $@.tmp $(URL)/$(notdir $@) && mv $@.tmp $@

#
# tests to run
#
tests-y += FileNotFoundTest
tests-y += CrapFileTest
tests-y += ImgNameErrorTest
tests-$(CONFIG_LIBCONFIG) += ValidImageTest
tests-y += InvOptsNoImg
tests-$(CONFIG_MONGOOSE) += InvOptsCheckWithWeb
tests-$(CONFIG_SURICATTA) += InvOptsCheckWithSur
tests-$(CONFIG_SIGNED_IMAGES) += InvSigNameCheck
tests-$(CONFIG_SIGNED_IMAGES) += ValidSigNameCheck

#
# file not found test
#
PHONY += FileNotFoundTest FileNotFound.swu
FileNotFoundTest: FileNotFound.swu FORCE $(if $(CONFIG_SIGNED_IMAGES), $(obj)/cacert.pem)
	$(call cmd,swu_check_assert_false)

#
# corrupt file test
#
PHONY += CrapFileTest
CrapFileTest: $(obj)/CrapFile.swu FORCE $(if $(CONFIG_SIGNED_IMAGES), $(obj)/cacert.pem)
	$(call cmd,swu_check_assert_false)

clean-files += CrapFile.swu
$(obj)/CrapFile.swu:
	$(Q)mkdir -p $(dir $@)
	$(Q)dd if=/dev/random of=$@ bs=1K count=1

#
# test of update file with image name in sw-description missmatch
#
PHONY += ImgNameErrorTest
ImgNameErrorTest: $(obj)/ImgNameError.swu FORCE $(if $(CONFIG_SIGNED_IMAGES), $(obj)/cacert.pem)
	$(call cmd,swu_check_assert_false)

%/hello.txt:
	$(Q)mkdir -p $(dir $@)
	$(Q)echo "Hello World" > $@

clean-dirs += ImgNameError
$(obj)/ImgNameError/sw-description:
	$(Q)mkdir -p $(dir $@)
	$(Q)printf "\
software =\n\
	{\n\
\n\
	version = \"0.1.1\";\n\
\n\
	test = {\n\
		hardware-compatibility: [ \"1\" ];\n\
	};\n\
\n\
	files:(\n\
\n\
		{\n\
		filename = \"FileDoesNotExist\";\n\
		path = \"/home/hello.txt\";\n\
		}\n\
\n\
	);\n\
\n\
	}\n\
" > $@

with_sig = $1 $(if $(CONFIG_SIGNED_IMAGES),$(addsuffix .sig, $1))

clean-files +=  ImgNameError.swu
$(obj)/ImgNameError.swu: $(call with_sig, $(obj)/ImgNameError/sw-description) $(obj)/ImgNameError/hello.txt
	$(call cmd,mkswu)

#
# Test of a valid *.swu file
#
PHONY += ValidImageTest
ValidImageTest: $(obj)/ValidImage.swu FORCE $(if $(CONFIG_SIGNED_IMAGES), $(obj)/cacert.pem)
	$(call cmd,swu_check_assert_true)

clean-dirs += ValidImage
$(obj)/ValidImage/sw-description:
	$(Q)mkdir -p $(dir $@)
	$(Q)printf "\
software =\n\
	{\n\
\n\
	version = \"0.2.2\";\n\
\n\
	test = {\n\
		hardware-compatibility: [ \"1\" ];\n\
	};\n\
\n\
	files:(\n\
\n\
		{\n\
		filename = \"hello.txt\";\n\
		path = \"/home/hello.txt\";\n\
$(if $(CONFIG_HASH_VERIFY),		sha256 = \"d2a84f4b8b650937ec8f73cd8be2c74add5a911ba64df27458ed8229da804a26\")\
		}\n\
\n\
	);\n\
\n\
	}\n\
" > $@

clean-files += ValidImage.swu
$(obj)/ValidImage.swu: $(call with_sig, $(obj)/ValidImage/sw-description) $(obj)/ValidImage/hello.txt
	$(call cmd,mkswu)

#
# invalid option test, no image given
#
PHONY += InvOptsNoImg
InvOptsNoImg: FORCE $(if $(CONFIG_SIGNED_IMAGES), $(obj)/cacert.pem)
	$(call cmd,swu_check_assert_false)

#
# invalid option test, web server with check
#
PHONY += InvOptsCheckWithWeb
InvOptsCheckWithWeb: FORCE $(if $(CONFIG_SIGNED_IMAGES), $(obj)/cacert.pem)
	$(call cmd,swu_check_inv_websrv)

#
# invalid option test, suricatta with check
#
PHONY += InvOptsCheckWithSur
InvOptsCheckWithSur: FORCE $(if $(CONFIG_SIGNED_IMAGES), $(obj)/cacert.pem)
	$(call cmd,swu_check_inv_suricatta)

clean-files += signer.pem cacert.pem
$(obj)/signer.pem $(obj)/cacert.pem:
	$(call cmd,download)

%/sw-description.sig :: %/sw-description $(obj)/signer.pem
	$(call cmd,sign_desc)


#
# invalid signer name
#
PHONY += InvSigNameCheck
InvSigNameCheck: $(obj)/ValidImage.swu $(obj)/InvSigNameCheck.cfg FORCE $(if $(CONFIG_SIGNED_IMAGES), $(obj)/cacert.pem)
	$(call cmd,swu_check_assert_false)

clean-files += InvSigNameCheck.cfg
$(obj)/InvSigNameCheck.cfg:
	$(Q)printf "\
globals: {\n\
	forced-signer-name = \"shall be different\";\n\
};\n\
" > $@

#
# valid signer name
#
PHONY += ValidSigNameCheck
ValidSigNameCheck: $(obj)/ValidImage.swu $(obj)/ValidSigNameCheck.cfg FORCE $(if $(CONFIG_SIGNED_IMAGES), $(obj)/cacert.pem)
	$(call cmd,swu_check_assert_true)

clean-files += ValidSigNameCheck.cfg
$(obj)/ValidSigNameCheck.cfg:
	$(Q)printf "\
globals: {\n\
        forced-signer-name = \"OpenSSL test S/MIME signer 1\";\n\
};\n\
" > $@

