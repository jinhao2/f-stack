/************************************************************************************************
* define stack's socket api for app user.
* dpdk funcs and ff_msg_rxtx func are used, bsd functions should not be used.
* 
************************************************************************************************/
#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <sched.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/un.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_pci.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_launch.h>
#include <rte_ethdev.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <rte_thash.h>

#include "ff_msg_rxtx.h"

#define my_procid 0
#define APP_SEND_DOWN_CTLMSG(obj) rte_ring_sp_enqueue( g_fstk_ctl[my_procid].ff_down_ctl_r,  (void*)(obj) )
#define APP_SEND_DOWN_WNDMSG(obj) rte_ring_sp_enqueue( g_fstk_ctl[my_procid].ff_down_wnd_r,  (void*)obj )
#define APP_RECV_UP_CTLMSG(obj,len) UxSktRecv(g_app_ctl[this_appid].app_up_ctl_us.sockfd, &g_fstk_ctl[my_procid].ff_up_ctl_us.addr, (char*)obj, len)
#define APP_RECV_UP_WNDMSG(obj) rte_ring_sc_dequeue( g_fstk_ctl[my_procid].ff_up_wnd_r,  (void**)obj)

#define THIS_SOCK_EVT_FD    g_fstk_ctl[this_appid].sock_evtfd
#define THIS_SOCK_EPOLL_FD  g_fstk_ctl[this_appid].sock_epfd

#define APP_EPOLL_TIMEOUT   10
#define APP_WAIT_EV_NB	    2
#define APP_EPOLL_FD 		(g_app_ctl[this_appid].ep_fd)

#define MAX_EVENT_NUM     2048
#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif

__thread int32_t ff_errno = 0;
__thread uint16_t this_appid = 0;
__thread uint16_t this_stkid = 0;
struct epoll_event  g_epoll_event[MAX_EVENT_NUM] ;
struct ff_app_ctl   g_app_ctl[FF_MAX_SOCKET_WORKER];
extern struct ff_fstk_ctl  g_fstk_ctl[8] ;
extern uint32_t g_fd_total;

extern int ff_creat_app_info( uint16_t appid );
extern int ff_get_newid();
extern int ff_get_sockst(int s);
extern int ff_set_sostat(int s, int stat);
extern int ff_get_err(int fd);

int app_set_attr(int id)
{
    int mid = 0;
    int cpuid = 0;
    cpu_set_t cpuset;
    pthread_t thread;
    char    pname[16];
    
    thread = pthread_self();
    CPU_ZERO(&cpuset);
    CPU_SET(id, &cpuset);
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) != 0)
    {
        RTE_LOG(INFO, USER1, "setcpu_affinity failed, errno %d.", errno );
        return -1;
    }

    snprintf(pname, sizeof(pname), "app%d_stk%d", this_appid, this_stkid);
    pthread_setname_np(thread, (const char*) pname);
    return 0;        
}

/*************************************************
 * Application worker registered to fstack.
 * return 0 means ok.
 * **********************************************/
int api_init() {
    //int id = ff_get_newid();
    ff_creat_app_info(0);
    return 0;
}

/*************************************************
 * sync socket api: send req, wait rsp, get result. througth eventfd.
 * 
 * **********************************************/
static inline int app_wait( int fd, int timeout ){
    struct epoll_event evs[APP_WAIT_EV_NB];
    eventfd_t val = 0;
    int rc = epoll_wait(fd, &evs[0], APP_EPOLL_EVENTS, timeout);
    if ( likely( rc>1 ) ){
        eventfd_read(fd, &val);
        return val;
    }
    if ( unlikely(rc < 0) ){
        RTE_LOG(INFO, USER1, "app_wait failed errno %d!\n", errno);
        return -1;
    }

    return 0;
}

/*********************************************************
 * app send SOCKET_EV to fstack, and waiting the response msg.
 * Sync Api: 
 *  app push ctlmsg into down_ctl_ring, then app blocking recvfrom() the unix socket app_up_ctl_us ;
 *  fstack poll the down_ctl_ring, do socket op, send up_ctl_msg by unix socket ff_up_ctl_us;
 *  app recv the msg successfully.
 * ******************************************************/
int api_socket(int domain, int type, int protocol){
    int32_t fd = -1;
    struct s_ff_ctl_msg* req = ff_get_sock_msg(DOWN_CTL_MSG, my_procid);
    struct s_ff_ctl_msg* rsp;
    assert(req != NULL);

    req->app_id = this_appid;
    req->event = SOCKET_EV;
    req->sock_fd = -1;
    req->req.sock_req.domain = domain;
    req->req.sock_req.type = type;
    req->req.sock_req.protocol = protocol;

    if ( APP_SEND_DOWN_CTLMSG(req)!= 0)
        return -1;
/***********************
    fd = ff_app_sync(THIS_SOCK_EPOLL_FD, APP_EPOLL_TIMEOUT ) ;
    if ( unlikely( fd<=0 ) ){
        ff_free_sock_msg(UP_CTL_MSG, my_procid, &req, 1);
        return -1;
    }
    APP_RECV_UP_CTLMSG(rsp);    
**********************/
	APP_RECV_UP_CTLMSG(&rsp, sizeof(rsp));
	if ( rsp->event == SOCKET_EV )
	    fd = rsp->sock_fd;
    ff_free_sock_msg(UP_CTL_MSG, my_procid, (void*)&rsp, 1 );
    return fd;
}

/*************************
 * app close a socket, send close command to stack then return.
 * return code: ==0 
 * **********************/
int api_close(int s)
{
    // send close ctl msg to fstack.
    struct s_ff_ctl_msg* req = ff_get_sock_msg(DOWN_CTL_MSG, my_procid);
    int ret = 0;

    req->app_id = this_appid;
    req->event = CLOSE_EV;
    req->sock_fd = s;

    if ( APP_SEND_DOWN_CTLMSG(req) != 1)
        ret = -1;
    
    return ret;
}


/*********************************************************
 * app send BIND_EV to fstack, and waiting the response msg.
 * not support  PR_ADDR/MSG_PEEK/MT_CONTROL/MSG_SOCALLBCK/MSG_WAITALL  
 * ******************************************************/
int api_bind(int s, const struct sockaddr *addr, socklen_t addrlen){
    struct s_ff_ctl_msg** rsp = NULL;
    struct s_ff_ctl_msg* req = ff_get_sock_msg(DOWN_CTL_MSG, my_procid);
    int ret = 0;

    req->app_id = this_appid;
    req->event = BIND_EV;
    req->sock_fd = s;
    memcpy(&req->req.bind_req.l_addr, addr, addrlen);
    req->req.bind_req.len =  addrlen;

    if ( APP_SEND_DOWN_CTLMSG(req) != 1)
        ret = -1;
    if ( unlikely( app_wait(APP_EPOLL_FD, APP_EPOLL_TIMEOUT) <=0 ) )
        ret = -1;
    
    APP_RECV_UP_CTLMSG((void*)(*rsp), sizeof(*rsp));
    if ( unlikely( (*rsp)->event != SOCKET_EV || (*rsp)->rsp.op_rsp.result < 0 )  )
        ret = -1;

    ff_free_sock_msg(UP_CTL_MSG, my_procid, (void**)rsp, 1 );
    return ret;
}

/*************************
 * app open a passive socket, return 
 * return code: ==0 means socket fd;  <0 means failed.
 * **********************/
int api_connect(int s, const struct sockaddr *addr, socklen_t addrlen)
{
    // send open+connect op msg to fstack.
    struct s_ff_ctl_msg* req = ff_get_sock_msg(DOWN_CTL_MSG, my_procid);
    int ret = 0;

    req->app_id = this_appid;
    req->event = CONNECT_EV;
    req->sock_fd = s;
    memcpy(&req->req.conn_req.p_addr, addr, addrlen);
    req->req.conn_req.len =  addrlen;

    if ( APP_SEND_DOWN_CTLMSG(req) != 1)
        ret = -1;
    
    return ret;
}
/*********************************************************
 * app send LISTEN_EV to fstack and return.
 * 
 * ******************************************************/
int api_listen(int s, int backlog){
    struct s_ff_ctl_msg* req = ff_get_sock_msg(DOWN_CTL_MSG, my_procid);

    req->app_id = this_appid;
    req->event = LISTEN_EV;
    req->sock_fd = s;
    req->req.listen_req.backlog = backlog;
    
    if (rte_ring_enqueue( g_fstk_ctl[my_procid].ff_down_ctl_r,  (void*)req ) != 1)
        return -1;
    
    return 0;
}

/*********************************************************
 * app send LISTEN_EV to fstack and return.
 * 
 * ******************************************************/
int api_accept(int s, int backlog){
        
    return 0;
}



/***************************************************
* app get it's own epollfd.
* 
***************************************************/
int api_epoll_create(){
	CList_init( &g_app_ctl[this_appid].fd_list, FF_MAX_SOCKET_NUM );
    return g_app_ctl[this_appid].ep_fd;
}

/***************************************************
* app add/del EPOLL_OUT event into epoll for one fd.
* 
***************************************************/
int api_epoll_add( int s, uint16_t evt){
    struct s_ff_ctl_msg** rsp = NULL;
    struct s_ff_ctl_msg* req = ff_get_sock_msg(DOWN_WND_MSG, my_procid);
    int ret = 0;

    req->app_id = this_appid;
    req->event = evt;
    req->sock_fd = s;

    if ( APP_SEND_DOWN_CTLMSG(req) != 1)
        ret = -1;
    
    return ret;
}

/************************************************************************
 * Get one bulk item from up ring.
 * There may be many same event for same fd.
 * *********************************************************************/
static inline int app_ring_burst( int bulk){
    struct s_ff_wnd_msg* objs[MAX_EVENT_NUM];
    uint32_t num, i=0;
    int s = 0;

    num = rte_ring_dequeue_burst(g_fstk_ctl[this_appid].ff_up_wnd_r, (void**)&objs, bulk, NULL);
    for (i = 0; i < num; i++) {
    	s = objs[i]->sock_fd;
    	if ( !ff_chk_sockfd(s) || ff_get_sockst(s)&objs[i]->event ){
    		continue;
    	}
        
        CList_Push( &g_app_ctl[this_appid].fd_list, s);
        ff_set_sostat(s, objs[i]->event);
        ff_epnb_inc(s);
    }
/*
    for (i = 0; i < (num & ((~(unsigned)0x3))); i+=4, idx+=4) {
        events[idx].events = objs[i]->event;
        events[idx].data.fd = objs[i]->sock_fd;

        events[idx+1].events = objs[i+1]->event;
        events[idx+1].data.fd = objs[i+1]->sock_fd;

        events[idx+2].events = objs[i+2]->event;
        events[idx+2].data.fd = objs[i+2]->sock_fd;

		events[idx+3].events = objs[i+3]->event;
        events[idx+3].data.fd = objs[i+3]->sock_fd;
	}
	switch (num & 0x3) {
		case 3:
            events[idx].events = objs[i]->event;
            events[idx++].data.fd = objs[i++]->sock_fd;
		case 2:
			events[idx].events = objs[i]->event;
            events[idx++].data.fd = objs[i++]->sock_fd;
		case 1:
			events[idx].events = objs[i]->event;
            events[idx++].data.fd = objs[i++]->sock_fd;
	}
*/
    ff_free_sock_msg(UP_WND_MSG, this_appid, (void**)&objs, num);
    return num;
}

/*********************************************************************
 * maxevents shoud be multi*4
 * App must recv all the bytes in specific socket's rx buffer. 
 * Only when one socket gets one packet into empty rx buffer, event signal will be sent.
 * *******************************************************************/
int api_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout_ms){
    uint32_t num, cnt, i;
    uint32_t fd = 0;
    CList_t * p = &g_app_ctl[this_appid].fd_list;
    
	cnt = CList_GetLen(p);
	if ( cnt < maxevents){
		app_ring_burst(2048);
	}
	cnt = CList_GetLen(p);
	if ( unlikely( !cnt ) ){
        if ( app_wait(APP_EPOLL_FD, APP_EPOLL_TIMEOUT) > 0){
            app_ring_burst( 2048);
        }
    }
    num = min(cnt, maxevents);
    for(i=0; i<num; i++){
        fd = CList_Pop(p);
        events[i].events = ff_get_sockst(fd);
        events[i].data.fd = fd;
	}
    return num;
}
/************************************************
 * app send wnd update msg down to fstack.
 * 
 * *********************************************/
static inline int app_snd_wndmsg(int fd, int evt, uint32_t mod ){
    struct s_ff_wnd_msg* wnd_msg = ff_get_sock_msg( DOWN_WND_MSG, my_procid );
    assert( wnd_msg != NULL );
    wnd_msg->sock_fd = this_appid;
    wnd_msg->event = evt;
    wnd_msg->sock_fd = fd;
    wnd_msg->mod_nb = mod; 
    APP_SEND_DOWN_WNDMSG(wnd_msg);
    return 0;
}
/************************************************
 * app copy data from rx, send rx_wnd event to fstack.
 * flags must be zero.
 * Only Support ET, App must recv data until return <= 0.
 * *********************************************/
int api_recv( int fd, void *buf, size_t len, int flags ){
    int32_t o_len = 0;
    
    o_len = ff_read_rx(fd, buf, len);
    if ( o_len > 0 ){
        app_snd_wndmsg(fd, RX_WND_EV, o_len);
    }
    
    return o_len;
}

int api_error(int fd){
	//return ff_get_err(fd);
	return ff_errno;
}

