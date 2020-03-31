#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <assert.h>
#include <sys/fcntl.h>
#include <unistd.h>

#include "ff_config.h"
#include "ff_api.h"

#define IP_BINDANY       24
//#define ff_nonblocking(s)  fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK)
#define MAX_EVENTS 512

/* kevent set */
struct kevent kevSet;
/* events */
struct kevent events[MAX_EVENTS];
/* kq */
int kq;
int sockfd;

char *g_DataPtr = NULL;
#define DATA_SIZE 1024*1024
#define CLIENT_IP       "11.10.10.216"
#define SERVER_IP       "10.10.10.11"

static unsigned long long recvbytes = 0;
time_t sec=0;
long   ns=0;
pid_t g_pid;

int ff_nonblocking(int s)
{
    int  nb;
    nb = 1;
    return ff_ioctl(s, 0x5421, &nb);
}

int loop(void *arg)
{
    /* Wait for events to happen */
    unsigned nevents = ff_kevent(kq, NULL, 0, events, MAX_EVENTS, NULL);
    unsigned i;

    for (i = 0; i < nevents; ++i) {
        struct kevent event = events[i];
        int clientfd = (int)event.ident;

        if (event.flags & EV_EOF) {
            ff_close(clientfd);
            printf("fd:%d recv \n", clientfd);
            
        } else if (event.filter == EVFILT_READ) {
            size_t readlen = ff_read(clientfd, g_DataPtr, DATA_SIZE);
            //printf("recv bytes %zu...,fd:%d\n", (size_t)readlen, clientfd);
            recvbytes += readlen;
			if ( (recvbytes & ((1<<27)))>0 )
			{
				ff_get_current_time(&sec, &ns);
				printf("[%d]%lu:%lu recv:%lu bytes\n", g_pid, sec, ns, recvbytes);
				recvbytes = 0;
			}
            
		} else if (event.filter == EVFILT_WRITE){
			//size_t writelen = ff_write(clientfd, g_DataPtr, DATA_SIZE);
			//printf("available to write...,fd:%d\n", clientfd);
						
        } else {
            printf("unknown event: %8.8X\n", event.flags);
        }
    }
}

int main(int argc, char * argv[])
{
    int ret = 0, i = 10;

	g_pid = getpid();
	g_DataPtr = malloc(DATA_SIZE);
	assert(g_DataPtr != NULL);
	memset(g_DataPtr, 0, DATA_SIZE);
		
    ff_init(argc, argv);

	struct sockaddr_in serv_addr;
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(80);
    serv_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
	assert((kq = ff_kqueue()) > 0);
	
	while (i>0)
	{
	    sockfd = ff_socket(AF_INET, SOCK_STREAM, 0);
	    printf("sockfd:%d\n", sockfd);
	    if (sockfd < 0)
	        printf("ff_socket failed\n");
		
		ff_nonblocking(sockfd);
		if (ff_setsockopt(sockfd, IPPROTO_IP, IP_BINDANY, (const void *) &value, sizeof(int)) == -1)
        {
			printf("setsockopt(IP_BINDANY) failed, errno %d\n", errno);
        }
		if ( ff_bind(sockfd, (const struct linux_sockaddr *)&clt_addr, sizeof(clt_addr)) < 0)
        {
            printf("ff_bind failed, errno %d.\n", errno);
        }		
	    ret = ff_connect(sockfd, (const struct linux_sockaddr *)&serv_addr, sizeof(serv_addr));
	    if (ret < 0) {
	        printf("ff_connect failed, errno %d.\n", errno);
	    }
		kevSet.data     = MAX_EVENTS;
	    kevSet.fflags   = 0;
	    kevSet.filter   = EVFILT_READ;
	    kevSet.flags    = EV_ADD;
	    kevSet.ident    = sockfd;
	    kevSet.udata    = NULL;

	    /* Update kqueue */
	    ff_kevent(kq, &kevSet, 1, NULL, 0, NULL);
		i--;
	}

	ff_get_current_time(&sec, &ns);
	printf("%lu:%lu recv:%lu bytes\n",  sec, ns, recvbytes);
	
    ff_run(loop, NULL);
	
    return 0;
}


