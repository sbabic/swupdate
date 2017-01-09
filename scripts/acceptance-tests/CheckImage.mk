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
# test commands for --check comand-line option
#
SWU_CHECK = ./swupdate -l 5 -c $(if $(strip $(filter-out FORCE,$<)),-i $<) >/dev/null 2>&1

quiet_cmd_swu_check_assert_false = RUN     $@
      cmd_swu_check_assert_false = $(SWU_CLEAN); if $(SWU_CHECK); then false; fi

quiet_cmd_swu_check_assert_true = RUN     $@
      cmd_swu_check_assert_true = $(SWU_CLEAN); $(SWU_CHECK)

quiet_cmd_swu_check_inv_websrv = RUN     $@
      cmd_swu_check_inv_websrv = $(SWU_CLEAN); if ./swupdate -l 5 -c -w "-document_root $(srctree)" >/dev/null 2>&1; then false; fi

quiet_cmd_swu_check_inv_suricatta = RUN     $@
      cmd_swu_check_inv_suricatta = $(SWU_CLEAN); if ./swupdate -l 5 -c -u "-t default -i 42 -u localhost:8080" >/dev/null 2>&1; then false; fi

quiet_cmd_mkswu = MKSWU   $@
      cmd_mkswu = mkdir -p $(dir $@); cd $(dir $<); for l in $(patsubst $(dir $<)%,%,$(filter-out FORCE,$^)); do echo "$$l"; done | cpio -ov -H crc > $(objtree)/$@

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

#
# file not found test
#
PHONY += FileNotFoundTest FileNotFound.swu
FileNotFoundTest: FileNotFound.swu FORCE
	$(call cmd,swu_check_assert_false)

#
# corrupt file test
#
PHONY += CrapFileTest
CrapFileTest: $(obj)/CrapFile.swu FORCE
	$(call cmd,swu_check_assert_false)

$(obj)/CrapFile.swu:
	$(Q)mkdir -p $(dir $@)
	$(Q)dd if=/dev/random of=$@ bs=1K count=1

#
# test of update file with image name in sw-description missmatch
#
PHONY += ImgNameErrorTest
ImgNameErrorTest: $(obj)/ImgNameError.swu FORCE
	$(call cmd,swu_check_assert_false)

%/hello.txt:
	$(Q)mkdir -p $(dir $@)
	$(Q)echo "Hello World" > $@

$(obj)/ImgNameError/sw-description:
	$(Q)mkdir -p $(dir $@)
	$(Q)printf "\
software =\n\
	{\n\
\n\
	version = \"0.1.1\";\n\
\n\
		hardware-compatibility: [ "1" ];\n\
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

$(obj)/ImgNameError.swu: $(obj)/ImgNameError/sw-description $(obj)/ImgNameError/hello.txt
	$(call cmd,mkswu)

#
# Test of a valid *.swu file
#
PHONY += ValidImageTest
ValidImageTest: $(obj)/ValidImage.swu FORCE
	$(call cmd,swu_check_assert_true)

$(obj)/ValidImage/sw-description:
	$(Q)mkdir -p $(dir $@)
	$(Q)printf "\
software =\n\
	{\n\
\n\
	version = \"0.2.2\";\n\
\n\
		hardware-compatibility: [ "1" ];\n\
\n\
	files:(\n\
\n\
		{\n\
		filename = \"hello.txt\";\n\
		path = \"/home/hello.txt\";\n\
		}\n\
\n\
	);\n\
\n\
	}\n\
" > $@

$(obj)/ValidImage.swu: $(obj)/ValidImage/sw-description $(obj)/ValidImage/hello.txt
	$(call cmd,mkswu)

#
# invalid option test, no image given
#
PHONY += InvOptsNoImg
InvOptsNoImg: FORCE
	$(call cmd,swu_check_assert_false)

#
# invalid option test, web server with check
#
PHONY += InvOptsCheckWithWeb
InvOptsCheckWithWeb: FORCE
	$(call cmd,swu_check_inv_websrv)

#
# invalid option test, suricatta with check
#
PHONY += InvOptsCheckWithSur
InvOptsCheckWithSur: FORCE
	$(call cmd,swu_check_inv_suricatta)

