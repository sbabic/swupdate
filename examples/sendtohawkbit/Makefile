
all: sendcmd

sendcmd: sendcmd.c
	${CC} -g -o sendcmd -I../../include sendcmd.c ../../ipc/lib.a -lpthread

clean:
	@rm -f sendcmd
