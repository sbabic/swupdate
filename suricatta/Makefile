# Copyright (C) 2014-2018 Stefano Babic <stefano.babic@swupdate.org>
#
# SPDX-License-Identifier:     GPL-2.0-only
obj-$(CONFIG_SURICATTA) += suricatta.o
ifneq ($(CONFIG_SURICATTA_HAWKBIT),)
obj-$(CONFIG_SURICATTA) += server_hawkbit.o
endif
ifneq ($(CONFIG_SURICATTA_LUA),)
obj-$(CONFIG_SURICATTA) += server_lua.o
endif
ifneq ($(CONFIG_SURICATTA_GENERAL),)
obj-$(CONFIG_SURICATTA_GENERAL) += server_general.o
endif

