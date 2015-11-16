
all: client

client: client.c
	${CC} -g -o client -I../../include client.c ../../ipc/lib.a -lpthread

clean:
	@rm -f client
