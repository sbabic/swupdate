
all: sendtohawkbit

sendtohawkbit: sendtohawkbit.c
	${CC} -g -o sendtohawkbit -I../../include sendtohawkbit.c ../../ipc/lib.a -lpthread

clean:
	@rm -f sendtohawkbit
