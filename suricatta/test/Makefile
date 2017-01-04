##
## Author: Christian Storm
## Copyright (C) 2016, Siemens AG
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

SURICATTA_TEST_DIR = $(abspath $(dir $(CURDIR)/$(word $(words $(MAKEFILE_LIST)),$(MAKEFILE_LIST))))
SURICATTA_DIR      = $(abspath $(SURICATTA_TEST_DIR)/../)
SWUPDATE_DIR       = $(abspath $(SURICATTA_TEST_DIR)/../../)

SURICATTA_INCLUDES = -I. -isystem $(SWUPDATE_DIR)/include -I$(SURICATTA_DIR)
SURICATTA_CFLAGS   = -Wall -Wextra -pedantic -fno-common -std=c99
SURICATTA_LDFLAGS  =
## transfer __mock_*() functions to according linker flags via rudimentary regex awk parsing
SURICATTA_LDMOCKS  = `/usr/bin/awk '/^[^(extern)|(static)]*([\*_[:alnum:]]+[[:blank:]]+)+[\*]*__wrap_/{where = match($$0,"__wrap_[_[:alnum:]]+"); if (RSTART > 0) { print "-Wl,--wrap=" substr($$0, RSTART+7, RLENGTH-7);}}' $(SURICATTA_TEST_DIR)/$@.c`
SURICATTA_LIBS     = -lcmocka -lcurl -ljson-c -lpthread -lcrypto -lssl -lz
SURICATTA_DEFINES  =  -DCONFIG_DEBUG -DCONFIG_SURICATTA_SSL -DUNIT_TESTING
SURICATTA_DEFINES  += -D_GNU_SOURCE -DHAVE_SIGNAL_H

ifeq ($(CC),clang)
	SURICATTA_CFLAGS += -Weverything -Wno-gnu-zero-variadic-macro-arguments -Wno-disabled-macro-expansion -Wno-switch-enum -Wno-bad-function-cast
endif

TEST_SRCS = $(wildcard test_*.c)
TEST_OBJS = $(addsuffix .o, $(basename $(TEST_SRCS)))
TARGETS   = $(basename $(TEST_SRCS))

SURICATTA_OBJS = $(SURICATTA_DIR)/server_hawkbit.o \
				 $(SURICATTA_DIR)/channel_hawkbit.o \
				 $(SURICATTA_DIR)/state.o \
				 $(SURICATTA_DIR)/suricatta.o

SURICATTA_SRCS = $(addsuffix .c, $(basename $(SURICATTA_OBJS)))

SWUPDATE_OBJS  = $(SWUPDATE_DIR)/ipc/network_ipc.o \
				 $(SWUPDATE_DIR)/corelib/swupdate_dict.o \
				 $(SWUPDATE_DIR)/corelib/progress_thread.o

COMPILE.c = $(CC) $(CFLAGS) $(SURICATTA_CFLAGS)  $(SURICATTA_DEFINES) $(SURICATTA_INCLUDES) -c
LINK.c    = $(CC) $(CFLAGS) $(SURICATTA_LDFLAGS) $(SURICATTA_LIBS)    $(SURICATTA_LDMOCKS)

EXECUTE_TEST = $(MAKE) --no-print-directory $(var)

all: run

## explicitly enumerate targets for zsh's completion, else
## simply use $(TARGETS): % : clean %.c $(SURICATTA_OBJS) $(SURICATTA_SRCS)
test_server_hawkbit test_json: % : clean %.c $(SURICATTA_OBJS) $(SURICATTA_SRCS)
	@$(info ## Compiling and Executing Test: $@)
	@$(COMPILE.c) $@.c -o $@.o
	## NOTE: assuming libubootenv.a is available in $(SWUPDATE_DIR)/ for linking
	## NOTE: assuming network_ipc.o is available in $(SWUPDATE_DIR)/ipc/ for linking
	## NOTE: assuming swupdate_dict.o is available in $(SWUPDATE_DIR)/corelib/ for linking
	## NOTE: assuming progress_thread.o is available in $(SWUPDATE_DIR)/corelib/ for linking
	@$(LINK.c) $(SWUPDATE_OBJS) $(SURICATTA_OBJS) $@.o -L$(SWUPDATE_DIR) -lubootenv -o $@
	@CMOCKA_MESSAGE_OUTPUT=TAP $(SURICATTA_TEST_DIR)/$@

run:
	@+$(foreach var,$(TARGETS),$(EXECUTE_TEST);)

%.o: %.c
	@$(COMPILE.c) $< -o $@

clean:
	@$(RM) $(TEST_OBJS) $(TARGETS) $(SURICATTA_OBJS)

.PHONY: clean all run $(TARGETS)
.SUFFIXES:
.NOTPARALLEL:
