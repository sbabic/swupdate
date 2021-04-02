# Makefile for swupdate
#
# Copyright (C) 2013 Stefano Babic <sbabic@denx.de>
#
# Based on busybox build system:
#
# Copyright (C) 1999-2005 by Erik Andersen <andersen@codepoet.org>
#
# SPDX-License-Identifier:     GPL-2.0-only

lib-y += \
	 swupdate-client.o \
	 swupdate-progress.o \
	 swupdate-hawkbitcfg.o \
	 swupdate-sendtohawkbit.o \
	 swupdate-sysrestart.o

# # Uncomment the next lines to integrate the compiling/linking of
# # any .c files placed alongside the above "official" tools in the
# # tools/ directory into SWUpdate's build system.
# TOOLS_DIR := $(dir $(word $(words $(MAKEFILE_LIST)),$(MAKEFILE_LIST)))
# TOOLS_SRC := $(wildcard $(TOOLS_DIR)/*.c)
# TOOLS_BIN := $(patsubst $(TOOLS_DIR)/%.c, %.o, $(TOOLS_SRC))
# lib-y += $(filter-out $(lib-y),$(TOOLS_BIN))
