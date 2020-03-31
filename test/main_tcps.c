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
#include <sys/ioctl.h>
#include <pthread.h>

#include "ff_config.h"
#include "ff_api.h"

#define MAX_EVENTS 512

/* kevent set */
struct kevent kevSet;
/* events */
struct kevent events[MAX_EVENTS];
/* kq */
int kq;
int sockfd;

char *g_DataPtr = NULL;
char* g_recvbuf = NULL;

#define DATA_SIZE 	65535
#define	SERVER_IP	"192.168.4.69"

static uint64_t  g_sendlen = 0;
time_t sec=0;
long   ns=0;

extern void *rte_malloc(const char *type, size_t size, unsigned align);
//#define ff_nonblocking(s)  fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK)
int ff_nonblocking(int s)
{
    int  nb;
    nb = 1;
    return ff_ioctl(s, 0x5421, &nb);
}

int set_sockopt(int fd)
{
	int bufsize, len;

	bufsize = 0;
	len = sizeof(bufsize);
	if (ff_getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (void *)&bufsize, &len)  == -1)
	{
		printf("getsockopt first failed.\n");
		return -1;
	}
	printf("getsockopt first sndbuf %d.\n", bufsize);

	if ( bufsize > 65535 )
		return 0;
	
	bufsize = 65535;
	if (ff_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const void *)&bufsize, sizeof(int)) == -1)
	{
		printf("setsockopt first failed.\n");
		return -2;
	}
	if (ff_getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (void *)&bufsize, &len)  == -1)
	{
		printf("getsockopt second failed.\n");
		return -3;
	}
	printf("getsockopt second sndbuf %d.\n", bufsize);
	
	return 0;
}

int new_tcp(int fd)
{
	int nclientfd = 0;
    
    nclientfd = ff_accept(fd, NULL, NULL);
    assert(nclientfd > 0);
	ff_nonblocking(nclientfd);
	ff_get_current_time(&sec, &ns);
	printf("%lu:%lu sent:%lu bytes\n",  sec, ns, g_sendlen);
	set_sockopt(nclientfd);
	
    kevSet.data     = 0;
    kevSet.fflags   = 0;
    kevSet.flags    = EV_ADD;
    kevSet.ident    = nclientfd;
    kevSet.udata    = NULL;
    kevSet.filter   = EVFILT_READ ;
    assert(ff_kevent(kq, &kevSet, 1, NULL, 0, NULL) == 0);

    kevSet.filter   = EVFILT_WRITE;
    assert(ff_kevent(kq, &kevSet, 1, NULL, 0, NULL) == 0);
    //ff_send( nclientfd, g_DataPtr, DATA_SIZE, 0);
    
    return 0;
}

static int g_outlen = 1;
int loop(void *arg)
{
    /* Wait for events to happen */
    unsigned nevents = ff_kevent(kq, NULL, 0, events, MAX_EVENTS, NULL);
    unsigned i;

    for (i = 0; i < nevents; ++i) {
        struct kevent event = events[i];
        int clientfd = (int)event.ident;
		size_t readlen = 0;
        /* Handle disconnect */
        if (event.flags & EV_EOF) {

            /* Simply close socket */
            ff_close(clientfd);
            printf("A client has left the server...,fd:%d\n", clientfd);
        } else if (clientfd == sockfd) {
            new_tcp(clientfd);

        } else if (event.filter == EVFILT_READ) {

            readlen = ff_recv(clientfd, g_recvbuf, DATA_SIZE, 0);
            //printf("ff_read readlen %d,fd:%d\n", readlen, clientfd);
        } else if (event.filter == EVFILT_WRITE){
		ssize_t writelen = ff_send(clientfd, g_DataPtr, DATA_SIZE, 0);
            //printf (" ff_send %d ok.\n", writelen);

		    //ssize_t writelen = ff_write(clientfd, g_DataPtr, g_outlen);	    
		    /*while ( writelen>0 )
		    {
		    	writelen = ff_write(clientfd, g_DataPtr, DATA_SIZE);
		    	//printf("%d\n", writelen);
		    }*/
		    
        } else {
            printf("unknown event: %8.8X\n", event.flags);
        }
    }
}

int main(int argc, char * argv[])
{
		g_recvbuf = malloc(DATA_SIZE);
		assert(g_recvbuf != NULL);
		memset(g_recvbuf, 0, DATA_SIZE);
		
    ff_init(argc, argv);
    g_DataPtr = rte_malloc("gData", DATA_SIZE, 8);
    //g_DataPtr = (char*)malloc(DATA_SIZE);
	assert(g_DataPtr != NULL);
	memset(g_DataPtr, 'a', DATA_SIZE);
    printf("g_DatatPtr %p.\n", g_DataPtr);
    
    sockfd = ff_socket(AF_INET, SOCK_STREAM, 0);
    printf("sockfd:%d\n", sockfd);
    if (sockfd < 0)
        printf("ff_socket failed\n");

    struct sockaddr_in my_addr;
    bzero(&my_addr, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(8080);
    my_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    int ret = ff_bind(sockfd, (struct linux_sockaddr *)&my_addr, sizeof(my_addr));
    if (ret < 0) {
        printf("ff_bind failed\n");
    }
	ff_nonblocking(sockfd);
	
	ret = ff_listen(sockfd, MAX_EVENTS);
    if (ret < 0) {
        printf("ff_listen failed\n");
    }

    kevSet.data     = MAX_EVENTS;
    kevSet.fflags   = 0;
    kevSet.filter   = EVFILT_READ;
    kevSet.flags    = EV_ADD;
    kevSet.ident    = sockfd;
    kevSet.udata    = NULL;

    assert((kq = ff_kqueue()) > 0);

    /* Update kqueue */
    ff_kevent(kq, &kevSet, 1, NULL, 0, NULL);
	ff_get_current_time(&sec, &ns);
	printf("%lu:%lu sent:%lu bytes\n",  sec, ns, g_sendlen);
    ff_run(loop, NULL);
    return 0;
}


