lib-y				+= installer.o \
				   network_thread.o \
				   stream_interface.o \
				   progress_thread.o \
				   parsing_library.o
lib-$(CONFIG_DOWNLOAD)		+= downloader.o
lib-$(CONFIG_MTD)		+= mtd-interface.o
lib-$(CONFIG_LUA)		+= lua_interface.o
lib-$(CONFIG_SIGNED_IMAGES)	+= verify_signature.o
lib-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt.o
lib-$(CONFIG_LIBCONFIG)		+= swupdate_settings.o
