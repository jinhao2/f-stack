TOPDIR=..

ifeq ($(FF_PATH),)
	FF_PATH=${TOPDIR}
endif

ifeq ($(FF_DPDK),)
	FF_DPDK=${TOPDIR}/dpdk/x86_64-native-linuxapp-gcc
endif

LIBS+= -L${FF_PATH}/lib -Wl,--whole-archive,-lfstack,--no-whole-archive
LIBS+= -L${FF_DPDK}/lib -Wl,--whole-archive,-ldpdk,--no-whole-archive
LIBS+= -Wl,--no-whole-archive -lrt -lm -ldl -lcrypto -lpthread -lnuma

TARGET="helloworld"
all:
	#cc -O0 -gdwarf-2 -g3  -I../lib -o tcpc main_tcpc.c ${LIBS}
	cc -O0 -gdwarf-2 -g3   -I../lib -o tcps main_tcps.c ${LIBS}
	cc -O0 -gdwarf-2 -g3   -I../lib -o udptest main_udp.c ${LIBS}

.PHONY: clean
clean:
	rm -f *.o tcps tcpc udptest
