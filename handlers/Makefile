# Note: the handlers are inserted automatically
# via a GCC constructor. To work, the linker should
# not drop the handler if it is not called.
# Handler can be called dinamically based
# on the received image type
lib-$(CONFIG_CFI)	+= flash_handler.o
lib-$(CONFIG_RAW)	+= raw_handler.o
lib-$(CONFIG_UBIVOL)	+= ubivol_handler.o
lib-$(CONFIG_LUASCRIPTHANDLER) += lua_scripthandler.o
lib-$(CONFIG_SHELLSCRIPTHANDLER) += shell_scripthandler.o
lib-$(CONFIG_RAW) += raw_handler.o
lib-$(CONFIG_UBOOT) += uboot_handler.o fw_env.o
