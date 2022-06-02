# Copyright (C) 2014-2018 Stefano Babic <sbabic@denx.de>
#
# SPDX-License-Identifier:     GPL-2.0-only
lib-$(CONFIG_SURICATTA) += suricatta.o common.o
ifneq ($(CONFIG_SURICATTA_HAWKBIT),)
lib-$(CONFIG_SURICATTA) += server_hawkbit.o
endif
ifneq ($(CONFIG_SURICATTA_LUA),)
lib-$(CONFIG_SURICATTA) += server_lua.o
endif
lib-$(CONFIG_SURICATTA_GENERAL) += server_general.o
