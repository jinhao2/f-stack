/******************************************************
 * App threads are isolated from f-stack worker, and communicated with f-stackers.
 * This file define rings, events, and rx/tx functions between App and f-stack.
 * Msg communication was implemented in this code.
 * ****************************************************/
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <assert.h>

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
#include <rte_memory.h>
#include <rte_lcore.h>
#include <rte_launch.h>
#include <rte_ethdev.h>
#include <rte_debug.h>
#include <rte_common.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <rte_thash.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_eth_bond.h>

#include "ff_app_rxtx.h"

#define FF_MAX_SOCKET_WORKER    8

#define MAX_KEEP 16
#define CTL_MSGPOOL_CACHE_MAX_SIZE  32
#define WND_MSGPOOL_CACHE_MAX_SIZE 256
#define MSGPOOL_SIZE ((rte_lcore_count()*(MAX_KEEP+WND_MSGPOOL_CACHE_MAX_SIZE))-1)
#define CTLPOOL_SIZE ((rte_lcore_count()*(MAX_KEEP+CTL_MSGPOOL_CACHE_MAX_SIZE))-1)

#define FF_SENDUP_CTLMSG(obj) ff_sendup_evt(obj, g_fstk_ctl[this_procid].ff_up_ctl_r)
#define FF_SENDUP_WNMSG(obj) ff_sendup_evt(obj, g_fstk_ctl[this_procid].ff_up_wnd_r)

// each fstack core has one struct ff_fstk_ctl.
struct ff_fstk_ctl  g_fstk_ctl[8] = {0};            //RTE_MAX_LCORE 

// each app socket worker has one struct ff_app_ctl.
struct ff_app_ctl   g_app_ctl[FF_MAX_SOCKET_WORKER] ={0};

extern struct lcore_conf lcore_conf;
uint16_t this_procid = lcore_conf.proc_id;
uint16_t this_appid = 0;

extern struct rte_ring * create_ring(const char *name, unsigned count, int socket_id, unsigned flags)j;

/*********************
 * each f-stack core has one up_ring/down_ring
 * each app socket worker has sockop_cache/wndmsg_cache.
 * input param: procid
 * ******************/
int ff_init_socket_info(int i )
{
    char	tmp_name[RTE_RING_NAMESIZE];
    int     is_primary = (rte_eal_process_type() == RTE_PROC_PRIMARY)? 1: 0;
 
        snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "ff_up_wnd_r_", i);
        g_fstk_ctl[i].ff_up_wnd_r = create_ring(tmp_name, APP_RING_SIZE, lcore_conf.socket_id, 0 );

        snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "ff_up_wnd_rop_", i);
        g_fstk_ctl[i].ff_up_ctl_r  = create_ring(tmp_name, APP_OP_RING_SIZE, lcore_conf.socket_id, 0 );

        snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "ff_down_ring_", i);
        g_fstk_ctl[i].ff_down_ring  = create_ring(tmp_name, APP_RING_SIZE, lcore_conf.socket_id, 0 );

        snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "ff_down_ringop_", i);
        g_fstk_ctl[i].ff_down_ring  = create_ring(tmp_name, APP_OP_RING_SIZE, lcore_conf.socket_id, 0 );

        snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "sockop_pool_", i);
        if ( is_primary )
            g_fstk_ctl[i].ff_ctlmsg_pool = rte_mempool_create(tmp_name, CTLPOOL_SIZE,
                                                    sizeof(struct s_ff_ctl_msg),
                                                    CTL_MSGPOOL_CACHE_MAX_SIZE, 0,
                                                    NULL, NULL,
                                                    NULL, NULL,
                                                    SOCKET_ID_ANY, 0);
        else
            g_fstk_ctl[i].ff_ctlmsg_pool = rte_mempool_lookup(tmp_name)
        if ( g_fstk_ctl[i].ff_ctlmsg_pool == NULL){
            rte_panic("create pool::%s failed!\n", tmp_name);
            return -1;
        }

        snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "wndmsg_pool_", i);
        if ( is_primary )
            g_fstk_ctl[i].ff_wndmsg_pool = rte_mempool_create(tmp_name, MSGPOOL_SIZE,
                                                    sizeof(struct s_ff_wnd_msg),
                                                    WND_MSGPOOL_CACHE_MAX_SIZE, 0,
                                                    NULL, NULL,
                                                    NULL, NULL,
                                                    SOCKET_ID_ANY, 0);
        else
            g_fstk_ctl[i].ff_wndmsg_pool = rte_mempool_lookup(tmp_name);
        if ( g_fstk_ctl[i].ff_wndmsg_pool == NULL){
            rte_panic("create pool::%s failed!\n", tmp_name);
            return -1;
        }

    return 0;
}

/***************************************
 * create app caches for each socket worker.
 * ************************************/
int ff_creat_app_info( uint16_t appid )
{
    struct epoll_event ev = {0};

    //RTE_ASSERT( appid < FF_MAX_SOCKET_WORKER );
    g_app_ctl[appid].app_ctlmsg_cache = rte_mempool_cache_create(CTL_MSGPOOL_CACHE_MAX_SIZE, SOCKET_ID_ANY);
    g_app_ctl[appid].app_wndmsg_cache = rte_mempool_cache_create(WND_MSGPOOL_CACHE_MAX_SIZE, SOCKET_ID_ANY);

    eventfd(g_app_ctl[appid].evt_fd, EFD_CLOEXEC|EFD_NONBLOCK);
    if ( g_app_ctl[appid].evt_fd <= 0 )
    {
        rte_panic("eventfd evt_fd failed errno %d!\n", errno);
        return -1;
    }
    g_app_ctl[appid].ep_fd = epoll_create1(EPOLL_CLOEXEC);
    if ( g_app_ctl[appid].ep_fd <= 0 ){
        rte_panic("epoll_create1 ep_fd failed errno %d!\n", errno);
        return -1;
    }
    ev.events = EPOLLIN|EPOLLERR;
    ev.data.u32 = appid;
    if (epoll_ctl(g_app_ctl[appid].ep_fd, EPOLL_CTL_ADD, g_app_ctl[appid].evt_fd, &ev) <0 ){
        rte_panic("epoll_ctl failed errno %d!\n", errno);
        return -1;
    }

    eventfd(g_app_ctl[appid].sock_evtfd, EFD_CLOEXEC|EFD_NONBLOCK);
    if ( g_app_ctl[appid].sock_evtfd <= 0 ){
        rte_panic("eventfd sock_evtfd failed errno %d!\n", errno);
        return -1;
    }
    g_app_ctl[appid].sock_epfd = epoll_create1(EPOLL_CLOEXEC);
    if ( g_app_ctl[appid].sock_epfd <= 0 ){
        rte_panic("epoll_create1 sock_epfd failed errno %d!\n", errno);
        return -1;
    }
    ev.events = EPOLLIN|EPOLLERR;
    ev.data.u32 = appid;                // means this is for socket open.
    if (epoll_ctl(g_app_ctl[appid].sock_epfd, EPOLL_CTL_ADD, g_app_ctl[appid].sock_evtfd, &ev) <0 ){
        rte_panic("epoll_ctl failed errno %d!\n", errno);
        return -1;
    }
    return 0;
}

/***************************************
 * fstack input new data into sockbuf, return the new input data.
 * return code: 
 *  0: no need sending up wnd msg.
 *  >0:need to send up wnd msg.
****************************************/
static inline int ff_sockbuf_input( struct sockbuff* sb, uint32_t nb ){
    sb->sb_input_nb += nb;
    if ( unlikely( sb->sb_input_nb == sb->sb_recvout_nb+nb ) ){
        return nb;           // should trigger new wnd update msg.
    }
    return 0;
}

static inline void ff_sockbuf_recvout( struct sockbuff* sb, uint32_t nb ){
    sb->sb_recvout_nb += nb;
}

/***************************************
 * get msg for communicate between fstack and socket worker.
 * In this function, up msg was allocated in fstack; down msg was allocated in app.
****************************************/
static inline void* ff_get_sock_msg(int16_t type, uint16_t procid ){
    struct ff_fstk_ctl* p_fstk = &g_fstk_ctl[procid];
    struct ff_app_ctl*  p_app = &g_app_ctl[procid];
    void* ptr = NULL;

    switch ( type ){
    case UP_CTL_MSG:
        rte_mempool_get(p_fstk->ff_ctlmsg_pool, &ptr);
        break;
    case UP_WND_MSG:
        rte_mempool_get(p_fstk->ff_wndmsg_pool, &ptr);
    case DOWN_CTL_MSG:
        rte_mempool_generic_get(p_fstk->ff_ctlmsg_pool, &ptr, 1, p_app->app_ctlmsg_cache);
    case DOWN_WND_MSG:
        rte_mempool_generic_get(p_fstk->ff_wndmsg_pool, &ptr, 1, p_app->app_wndmsg_cache);
    default:
        void* ptr = NULL;
        break;
    }
    return ptr;
}

/***************************************
 * free msg for communicate between fstack and socket worker.
 * In this function, up msg was freed in app; down msg was freed in fstack.
****************************************/
static inline int ff_free_sock_msg( int16_t type, uint16_t procid, void** obj, uint32_t num){
    struct ff_fstk_ctl* p_fstk = &g_fstk_ctl[procid];
    struct ff_app_ctl*  p_app = &g_app_ctl[procid];

    switch ( type ){
    case UP_CTL_MSG:
        rte_mempool_generic_put(p_fstk->ff_ctlmsg_pool, obj, num, p_app->app_ctlmsg_cache);
        break;
    case UP_WND_MSG:
        rte_mempool_generic_put(p_fstk->ff_wndmsg_pool, obj, num, p_app->app_wndmsg_cache);
    case DOWN_CTL_MSG:
        rte_mempool_put_bulk(p_fstk->ff_ctlmsg_pool, obj, num);
    case DOWN_WND_MSG:
        rte_mempool_put_bulk(p_fstk->ff_wndmsg_pool, obj, num);
    default:
        return -1;
        break;
    }
    return 0;
}

#define FF_SENDUP_SIGNAL_THRESHOLD  8

StackList_t g_ff_socket_ctl = {0};
struct socket** g_ff_socket_vector = NULL;
volatile uint64_t*  g_ff_in_nb_vec = NULL;              /* tcp push into the sb bytes, added by fstack */
volatile uint64_t*  g_ff_out_nb_vec = NULL;		        /* app read out from the sb bytes, added by app*/ 

int ff_init_fd_ctl(uint32_t maxsockets){
    int error = 0;

    g_ff_socket_vector = (struct socket**)rte_malloc("socket addr array", maxsockets, sizeof(void*));
    assert( g_ff_socket_vector );
    
    stklist_init(&g_ff_socket_ctl, maxsockets);
    for(int i=maxsockets-1; i>0; i--){
        stklist_push(&g_ff_socket_ctl, i);
    }

    g_ff_in_nb_vec = ( uint64_t ) rte_malloc("socket in_nb vec", maxsockets, sizeof(uint64_t));
    assert( g_ff_in_nb_vec );
    g_ff_out_nb_vec = ( uint64_t ) rte_malloc("socket out_nb vec", maxsockets, sizeof(uint64_t));
    assert(g_ff_out_nb_vec);

    return 0;
}

// curthread is the fstack thread.
/**************************************************************************
 * fstack get struct socket from g_ff_socket_vector indexed by fd.
 * 
 * In BSD socket = (struct socket *)curthread->td_proc->p_fd->fd_files->fdt_ofiles[fd].fde_file->f_data;
 * ************************************************************************/
static inline struct socket* ff_get_sock_obj( int s ){
    return g_ff_socket_vector[s];
}

/**************************************************************************
 * fstack enqueue into ring, and try to send event signal.
 * 
 * In BSD socket = (struct socket *)curthread->td_proc->p_fd->fd_files->fdt_ofiles[fd].fde_file->f_data;
 * ************************************************************************/
static inline int ff_sendup_evt(void* msg, struct rte_ring* up_ring){
    //uint32_t pre_num, cur_num;
    //pre_num = rte_ring_count(up_ring);
    int ret = rte_ring_sp_enqueue(up_ring, (void*) msg);
    if ( unlikely( res!=1 ) ){
        ff_free_sock_msg(UP_CTL_MSG, this_procid, *msg, 1);
        return -1;
    }
    if ( likely( 1 == rte_ring_count(up_ring) ) ){
        eventfd_write(g_app_ctl[appid].evt_fd, 1);
    }
    return ret;
}

/*************************
 * fstack recv SOCKET_EV, create new socket.
 * get socket from global vector and allocate new struct socket.
 * **********************/
int ff_new_socket(int domain, int type, int protocol){
    uint32_t fd = 0;
    struct socket *so;

    fd = stklist_pop(&g_ff_socket_ctl);
    if ( unlikely( g_ff_socket_vector[fd]==NULL) ){
        int error = socreate(domain, &so, type, protocol, NULL, curthread);
        if ( unlikely(error!=0) ){
            stklist_push(&g_ff_socket_ctl, fd);
            return -1;
        }
    }
    g_ff_socket_vector[fd] = so;
    return fd;
}

/*****************************************************
 * fstack recv socket open event, get new fd, 
 * update the msg, sent it back. no need getting new s_ff_ctl_msg.
 * **************************************************/
static int ff_sock_ev_proc(struct s_ff_ctl_msg* sock_req){
    int fd = 0;
    struct s_ff_ctl_msg* rsp = NULL;
    fd = ff_new_socket(sock_req->req.sock_req.domain, sock_req->req.sock_req.type, sock_req->req.sock_req.protocol);
    if ( unlikely(fd <= 0) ){
        return -1;
    }
    rsp = (struct s_ff_ctl_msg*)sock_req; // reuse sock request msg.
    rsp->app_id = this_appid;
    rsp->event = SOCKET_EV;
    rsp->sock_fd = rsp->rsp.op_rsp.result = fd;
    if ( unlikely(ff_sendup_evt((void*) rsp， g_fstk_ctl[this_procid].ff_up_ctl_r) != 1)){
        RTE_LOG(INFO, sockmsg, "ff_sock_ev_proc enqueue failed." );        
        return -1;
    }

    return 0;
}

/*****************************************************
 * fstack do connect action.
 * rsp message will be sent after connectting completed.
 * 
 * **************************************************/
static int ff_connect_ev_proc(struct s_ff_ctl_msg* req){

    struct sockaddr_storage bsdaddr;
    struct socket* so = NULL;

    linux2freebsd_sockaddr(req->req.conn_req.laddr , req->req.conn_req.len, (struct sockaddr *)&bsdaddr);
    so = ff_get_sock_obj( con_req->sock_fd );
    if (so->so_state & SS_ISCONNECTING) {
		return (EALREADY);
    }
    soconnect(so, bsdaddr, curthread);
    so->so_state &= ~SS_ISCONNECTING;
    ff_free_sock_msg(DOWN_CTL_MSG, this_procid, &req, 1);
    return 0;
}

/*****************************************************
 * fstack send connect rsp to app after connect op completed.
 * should be called by tcp_input routing when connect OK, connect failed, timeout.
 * **************************************************/
int ff_connect_ok(int fd, int res){
    struct s_ff_ctl_msg* rsp = ff_get_sock_msg( UP_CTL_MSG, this_procid);

    rsp->app_id = this_procid;
    rsp->event = CONNECT_EV;
    rsp->sock_fd = fd;
    rsp->rsp.op_rsp.result = res;
    if ( unlikely( FF_SENDUP_CTLMSG(rsp) != 1)){
        RTE_LOG(INFO, sockmsg, "ff_connect_ok enqueue failed." );
        return -1;
    }

    return 0;
}

/************************************************************
 * fstack update all the rx buffer.
 * not support  PR_ADDR/MSG_PEEK/MT_CONTROL/MSG_SOCALLBCK/MSG_WAITALL  
 * mp0 == NULL, 
 * support MT_OOBDATA 
 * refer to soreceive_stream().
 * ********************************************************/
int ff_rxwnd_update( int sock_fd, uint32_t mod_nb ){
    int error = 0;
    int len = mod_nb;
    struct socket* so = ff_get_sock_obj(sock_fd);
    /*
	 * Remove the delivered data from the socket buffer unless we
	 * were only peeking.
	 */
	if (len > 0)
		sbdrop_locked(sb, len);

	/* Notify protocol that we drained some data. */

	VNET_SO_ASSERT(so);
	(*so->so_proto->pr_usrreqs->pru_rcvd)(so, flags);

	//SOCKBUF_LOCK_ASSERT(sb);
	SBLASTRECORDCHK(sb);
	SBLASTMBUFCHK(sb);
	//SOCKBUF_UNLOCK(sb);
	//sbunlock(sb);
	return 0;
}

/**************************************************
 * fstack process one req from app.
 * called by fstack's main circle.
 * ***********************************************/
int ff_proc_req_msg(struct s_ff_ctl_msg* req){
    switch(req->event){
        case SOCKET_EV:
            ff_sock_ev_proc( (struct s_ff_ctl_msg*) req);
            break;
        case CONNECT_EV:
            ff_connect_ev_proc( (struct s_ff_ctl_msg*) req);
            break;
        case BIND_EV:
            
            break;
        default:
            break;
    }
}
#define MAX_DOWN_RING_BURST 64
#define MAX_DOWN_RING_MSG   1024
/*********************************************************
 * Process the down wnd msg from app to f-stack.
 * Try to update same socket once when continuous same socket's wnd msg.
 * ******************************************************/
void ff_proc_wnd_ring(){
	struct s_ff_wnd_msg* ele_burst[MAX_DOWN_RING_BURST];
	int i = 0, nb_rx = 0, count = 0;
    uint32_t wnd_mod = 0;
    struct s_ff_wnd_msg* pre_msg = NULL;

    if ( unlikely( rte_ring_empty(g_fstk_ctl[this_procid].ff_down_wnd_r) ) ){
        return ;
    }

	nb_rx = rte_ring_dequeue_burst(g_fstk_ctl[this_procid].ff_down_wnd_r, ele_burst, MAX_DOWN_RING_BURST, NULL);
    while ( nb_rx ){
        for ( i=0; i<nb_rx; i++){
            if( wnd_mod == 0 || !pre_msg || pre_msg->sock_fd == ele_burst[i]->sock_fd ){
                wnd_mod += ele_burst[i]->mod_nb;
                pre_msg = ele_burst[i];
            }
            else if ( pre_msg->sock_fd != ele_burst[i]->sock_fd ){
                // update continous same socket wnd.
                ff_rxwnd_update( pre_msg->sock_fd, wnd_mod );
                wnd_mod = 0;
                pre_msg = NULL;
            }
        }
        count += nb_rx;
        if ( unlikely(count >= MAX_DOWN_RING_MSG ) )
            break;
        nb_rx = rte_ring_dequeue_burst(g_fstk_ctl[this_procid].ff_down_wnd_r, ele_burst, MAX_PKT_BURST, NULL);
    }
}

/*********************************************************
 * ff_proc_ctl_ring called by fstack main loop.
 * Process the down ctl msg from app to f-stack.
 * ******************************************************/
void ff_proc_ctl_ring(){
	struct s_ff_ctl_msg* ele_burst[MAX_DOWN_RING_BURST];
	int i = 0, nb_rx = 0, count = 0;

    if ( unlikely( rte_ring_empty(g_fstk_ctl[this_procid].ff_down_ctl_r) ) ){
        return ;
    }

	nb_rx = rte_ring_dequeue_burst(g_fstk_ctl[this_procid].ff_down_ctl_r, ele_burst, MAX_DOWN_RING_BURST, NULL);
    while ( nb_rx ){
        for ( i=0; i<nb_rx; i++){
            ff_proc_req_msg( ele_burst[i] );
        }
        ff_free_sock_msg(g_fstk_ctl[this_procid].ff_wndmsg_pool, ele_burst, nb_rx);
        count += nb_rx;
        if ( unlikely(count >= MAX_DOWN_RING_MSG ) )
            break;
        
        nb_rx = rte_ring_dequeue_burst(g_fstk_ctl[this_procid].ff_down_wnd_r, ele_burst, MAX_PKT_BURST, NULL);
    }
}

/*************************************************
 * fstack wakeup the waiting queue after rx buffer is large enough
 * Called by tcp_input routing, how only if rx buffer from 0 to nonezero.
 * len: the new added length.
 * **********************************************/
int ff_tcp_input_ok(struct sockbuf* sb, int len){
    int16_t mod;
    int fd, num;
    struct s_ff_wnd_msg* p_msg;

    mod = ff_sockbuf_input(sb, len);
    if( !mod )
        return 0;
    p_msg = ff_get_sock_msg( UP_WND_MSG, this_procid );
    assert(p_msg);

    fd = sb->fd_wait;                      //  waitfd replace "struct	selinfo sb_sel".
    p_msg->app_id = this_appid;
    p_msg->event = RX_WND_EV;
    p_msg->mod_nb = mod;
    p_msg->sock_fd = fd;

    if ( unlikely(ff_sendup_evt((void*) p_msg， g_fstk_ctl[this_procid].ff_up_ctl_r) != 1)){
        RTE_LOG(INFO, sockmsg, "ff_tcp_input_ok enqueue failed." );        
        return -1;
    }
    return 0;
}

/*******************************************************App API***************************************************************/
#define APP_SEND_DOWN_CTLMSG(obj) rte_ring_sp_enqueue( g_fstk_ctl[this_procid].ff_down_ctl_r,  (void*)obj )
#define APP_SEND_DOWN_WNDMSG(obj) rte_ring_sp_enqueue( g_fstk_ctl[this_procid].ff_down_wnd_r,  (void*)obj )
#define APP_RECV_UP_CTLMSG(obj) rte_ring_sc_dequeue( g_fstk_ctl[this_procid].ff_up_ctl_r,  (void**)obj)
#define APP_RECV_UP_WNDMSG(obj) rte_ring_sc_dequeue( g_fstk_ctl[this_procid].ff_up_wnd_r,  (void**)obj)

#define THIS_SOCK_EVT_FD    g_fstk_ctl[this_appid].sock_evtfd
#define THIS_SOCK_EPOLL_FD  g_fstk_ctl[this_appid].sock_epfd

#define APP_EPOLL_TIMEOUT   10
#define APP_EPOLL_EVENTS    2

int ff_app_sync( int fd, int timeout ){
    struct epoll_event evs[APP_EPOLL_EVENTS] = {0};
    int val = 0;
    int rc = epoll_wait(fd, &evs, APP_EPOLL_EVENTS, timeout);
    if ( likely( rc>1 ) ){
        eventfd_read(fd, &val);
        return val;
    }
    if ( unlikely(rc < 0) ){
        RTE_LOG(INFO, appsock, "epoll_wait failed errno %d!\n", errno);
        return -1;
    }

    return 0;
}

/*********************************************************
 * app send SOCKET_EV to fstack, and waiting the response msg.
 * Sync Api
 * ******************************************************/
int ff_app_socket(int domain, int type, int protocol){
    int32_t fd = -1;
    struct s_ff_ctl_msg* req = ff_get_sock_msg(DOWN_CTL_MSG, this_procid);
    struct s_ff_ctl_msg** rsp = NULL;

    req->app_id = this_appid;
    req->event = SOCKET_EV;
    req->sock_fd = -1;
    req->req.sock_req.domain = domain;
    req->req.sock_req.type = type;
    req->req.sock_req.protocol = protocol;

    if ( APP_SEND_DOWN_CTLMSG(req)!= 1)
        return -1;
    fd = ff_app_sync(THIS_SOCK_EPOLL_FD, APP_EPOLL_TIMEOUT ) ;
    if ( unlikely( fd<=0 ) ){
        ff_free_sock_msg(UP_CTL_MSG, this_procid, &req, 1);
        return -1;
    }
    APP_RECV_UP_CTLMSG(rsp);
    ff_free_sock_msg(UP_CTL_MSG, this_procid, rsp, 1 );
    return fd;
}

/*********************************************************
 * app send BIND_EV to fstack, and waiting the response msg.
 * not support  PR_ADDR/MSG_PEEK/MT_CONTROL/MSG_SOCALLBCK/MSG_WAITALL  
 * ******************************************************/
int ff_app_bind(int s, const struct sockaddr *addr, socklen_t addrlen){
    struct s_ff_ctl_msg** rsp = NULL;
    struct s_ff_ctl_msg* req = ff_get_sock_msg(DOWN_CTL_MSG, this_procid);
    int ret = 0;

    req->app_id = this_appid;
    req->event = BIND_EV;
    req->sock_fd = s;
    memcpy(req->req.bind_req.l_addr, addr, addrlen);
    req->req.bind_req.len =  addrlen;

    if ( APP_SEND_DOWN_CTLMSG(req) != 1)
        ret = -1;
    if ( unlikely( ff_app_sync(this_appid) <=0 ) )
        ret = -1;
    
    APP_RECV_UP_CTLMSG(rsp);
    if ( unlikely( *rsp->event != SOCKET_EV || *rsp->rsp.op_rsp.result < 0 )  )
        ret = -1;

    ff_free_sock_msg(UP_CTL_MSG, this_procid, rsp, 1 );
    return ret;
}

/*************************
 * app open a passive socket, return 
 * return code: >0 means socket fd;  <=0 means failed.
 * **********************/
int ff_app_connect()
{
    // send open+connect op msg to fstack.
    struct s_ff_ctl_msg** rsp = NULL;
    struct s_ff_ctl_msg* req = ff_get_sock_msg(DOWN_CTL_MSG, this_procid);
    int ret = 0;

    req->app_id = this_appid;
    req->event = CONNECT_EV;
    req->sock_fd = s;
    memcpy(req->req.bind_req.l_addr, addr, addrlen);
    req->req.bind_req.len =  addrlen;

    if ( APP_SEND_DOWN_CTLMSG(req) != 1)
        ret = -1;
    
    return ret;
}

/*********************************************************
 * app send LISTEN_EV to fstack and return.
 * 
 * ******************************************************/
int ff_app_listen(int s, int backlog){
    struct s_ff_ctl_msg* rsp = NULL;
    struct s_ff_ctl_msg* req = ff_get_sock_msg(DOWN_CTL_MSG, this_procid);

    req->app_id = this_appid;
    req->event = LISTEN_EV;
    req->sock_fd = s;
    req->req.listen_req.backlog = backlog;
    
    if (rte_ring_enqueue( g_fstk_ctl[this_procid].ff_down_ring,  (void*)req ) != 1)
        return -1;
    
    return 0;
}

int ff_app_epolladd( int s, int epollfd ){
    struct epoll_event ev = {0};

    ev.events = EPOLLIN|EPOLLERR;
    ev.data.u32 = appid;
    if (epoll_ctl(g_app_ctl[appid].ep_fd, EPOLL_CTL_ADD, g_app_ctl[appid].evt_fd, &ev) <0 ){
        rte_panic("epoll_ctl failed errno %d!\n", errno);
        return -1;
    }
}

#define MAX_EVENT_NUM     2048
struct epoll_event  g_epoll_event[MAX_EVENT_NUM] = {0};

/************************************************************************
 * Get one bulk item from up ring.
 * 
 * *********************************************************************/
static inline int ff_app_get_evts(struct epoll_event *events, int maxevents){
    struct s_ff_wnd_msg* objs[MAX_EVENT_NUM];
    uint32_t num, i, evt_id = 0, bulknum;

    bulknum = (maxevents > MAX_EVENT_NUM) ? MAX_EVENT_NUM : maxevents;
    num = rte_ring_dequeue_burst(g_fstk_ctl[this_appid].ff_up_wnd_r, (void* const *)&objs, maxevents, NULL);
    for(i=0; i<num; i+=4){
        events[evt_id++].events = objs[i]->event;
        events[evt_id++].data.fd = objs[i]->sock_fd;

        events[evt_id++].events = objs[i+1]->event;
        events[evt_id++].data.fd = objs[i+1]->sock_fd;

        events[evt_id++].events = objs[i+2]->event;
        events[evt_id++].data.fd = objs[i+2]->sock_fd;

        events[evt_id++].events = objs[i+3]->event;
        events[evt_id++].data.fd = objs[i+3]->sock_fd;
    }
    for( i-=4; i<num; i++){
        events[evt_id++].events = objs[i]->event;
        events[evt_id++].data.fd = objs[i]->sock_fd;
    }
    ff_free_sock_msg(UP_WND_MSG, &objs, num);
    return num;
}

/*********************************************************************
 * maxevents shoud be multi*4
 * App must recv all the bytes in specific socket's rx buffer. 
 * Only when one socket gets one packet into empty rx buffer, event signal will be sent.
 * *******************************************************************/
int ff_app_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout_ms){
    uint32_t num;
    
    num = ff_app_get_evts(events, maxevents);
    if ( unlikeyly( !num )  )
        ff_app_sync(this_appid, timeout_ms);
    num = ff_app_get_evts(events, maxevents);

    return num;
}

/************************************************
 * app copy data from rx buffer as much as 
 * flags must be zero.
 * *********************************************/
static inline int ff_app_copy_stream(void* buf, size_t len, struct sockbuf* sb){
    struct uio auio;
    struct iovec iov;
    uint32_t o_len = 0;

    iov.iov_base = buf;
    iov.iov_len = len;

    auio.uio_iov = &iov;
	auio.uio_iovcnt = 1;
	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_td = curthread;
	auio.uio_offset = 0;
	auio.uio_resid = len;

    o_len = min(len, sbavail(sb));
    if ( unlikely(m_mbuftouio(auio, sb->sb_mb, o_len)) ){
        return -1;
    }
    // m_freem(sbcut_internal(sb, len));   // fstack should do this op.
    return o_len;
}

static inline int ff_app_send_wndmsg(int fd, int evt, uint32_t mod ){
    struct s_ff_wnd_msg* wnd_msg = ff_get_sock_msg( DOWN_WND_MSG, this_procid );
    wnd_msg->sock_fd = this_appid;
    wnd_msg->event = evt;
    wnd_msg->sock_fd = fd;
    wnd_msg->mod_nb = mod; 
    rte_ring_sp_enqueue(g_fstk_ctl[this_procid].ff_down_ring, (void*) wnd_msg);
    return 0;
}

/************************************************
 * app copy data from rx, send rx_wnd event to fstack.
 * flags must be zero.
 * *********************************************/
int ff_app_recv( int s, void *buf, size_t len, int flags ){
    int32_t o_len = 0;

    struct socket* so = ff_get_sock_obj(fd);
    RTE_ASSERT(so!=NULL);
    struct sockbuf* rx_sb = &so->so_rcv;
    RTE_ASSERT(rx_sb!=NULL);

    o_len = ff_app_copy_stream(buf, size, rx_sb);
    if (  unlikely( o_len<0)  )
        return -1;
    ff_app_send_wndmsg(s, RX_WND_EV, o_len);
    return o_len;
}

