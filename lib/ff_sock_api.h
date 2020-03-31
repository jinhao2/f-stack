/********************************************************************
 * external socket api for application.
 * 
 * 
 * ****************************************************************/
#ifndef	_FF_SOCK_API_
#define _FF_SOCK_API_

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>
#include <sys/epoll.h>

int api_init();
int ff_init(int argc, char * const argv[]);
int api_socket(int domain, int type, int protocol);
int api_bind(int s, const struct sockaddr *addr, socklen_t addrlen);
int api_connect(int s, const struct sockaddr *addr, socklen_t addrlen);
int api_recv( int fd, void *buf, size_t len, int flags );

int api_epoll_add( int s, uint16_t evt);
int api_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout_ms);
int api_epoll_create();

#ifdef __cplusplus
}
#endif
#endif
