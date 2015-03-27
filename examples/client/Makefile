
all: client

client: client.c
	$(CROSS_COMPILE)gcc -g -o client -I../../include client.c ../../ipc/lib.a -lpthread

clean:
	@rm -f client
