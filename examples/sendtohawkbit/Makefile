
all: sendtohawkbit hawkbitcfg

%: %.c
	$(CC) -g -o $@ $< -I../../include ../../ipc/lib.a -lpthread

clean:
	@rm -f sendtohawkbit
