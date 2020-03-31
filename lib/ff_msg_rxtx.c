/***************************************************************************************
 * App threads are isolated from f-stack worker, and communicated with f-stackers.
 * This file define rings, events, and rx/tx functions between App and f-stack.
 * No using bsd interal function, and compiled with system default libs.
 * If bsd's funcs are needed, they should be encapsulated in ff_syscal_wrapper and called indirectly.
 * ************************************************************************************/
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/un.h>
#include <fcntl.h>

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
#include <rte_errno.h>

#include "ff_dpdk_if.h"
#include "ff_dpdk_pcap.h"
#include "ff_dpdk_kni.h"
#include "ff_config.h"
#include "ff_veth.h"
#include "ff_host_interface.h"
#include "ff_msg.h"
#include "ff_api.h"
#include "ff_memory.h"
#include "ff_msg_rxtx.h"

#define WND_MSGPOOL_CACHE_SIZE  256
#define MSGPOOL_SIZE ((rte_lcore_count()*(512+WND_MSGPOOL_CACHE_SIZE)))
#define CTL_MSGPOOL_CACHE_SIZE  8
#define CTLPOOL_SIZE ((rte_lcore_count()*(32+CTL_MSGPOOL_CACHE_SIZE)))
#define cur_appid 0
#define FF_SENDUP_CTLMSG(obj,len) UxSktSend(\
			g_fstk_ctl[this_procid].ff_up_ctl_us.sockfd,\
			&g_app_ctl[cur_appid].app_up_ctl_us.addr,\
			(char*)(obj), (len))
#define FF_SENDUP_WNDMSG(obj) ff_sendup_evt( (obj), g_fstk_ctl[this_procid].ff_up_wnd_r)
#define FF_SAME_WNDMSG(a,b) ( (a)->event == (b)->event && (a)->sock_fd == (b)->sock_fd )

const char ff_ux_sockf[] = "./ff_ux_sock";
const char app_ux_sockf[] = "./app_ux_sock";

// each fstack core has one struct ff_fstk_ctl.    //RTE_MAX_LCORE 
struct ff_fstk_ctl  g_fstk_ctl[8];
extern struct ff_app_ctl   g_app_ctl[FF_MAX_SOCKET_WORKER];
extern struct lcore_conf lcore_conf;
extern __thread uint16_t this_appid;
extern __thread int32_t ff_errno;

uint32_t g_fd_total = 4096;
uint32_t this_procid = 0;           //lcore_conf.proc_id;

extern struct rte_ring * create_ring(const char *name, unsigned count, int socket_id, unsigned flags);
extern void* ff_socreate(int domain, int type, int protocol);
extern int ff_soconnect(void* so, const struct linux_sockaddr *l_addr, socklen_t addrlen );
extern int ff_tcp_rxupdate(void* p, int len, void* p_end);
extern void* ff_get_rcvbuf_start(void* so);
extern int ff_sorxcopy(void* so, void* i_ptr, char*buf, int len, int offset);
extern void** ff_get_rxbuf_fdinfo(void* so);
extern void** ff_get_txbuf_fdinfo(void* so);
extern int ff_soclose(void* so);

StackList_t g_ff_socket_ctl = {0};
struct s_fd_sockbuf** g_ff_socket_vector = NULL;
//volatile uint64_t*  g_ff_in_nb_vec = NULL;              /* tcp push into the sb bytes, added by fstack */
//volatile uint64_t*  g_ff_out_nb_vec = NULL;		        /* app read out from the sb bytes, added by app*/ 

int ff_init_fd_ctl(uint32_t maxsockets){
    int error = 0;
    int i = 0;
    g_ff_socket_vector = (struct s_fd_sockbuf**)rte_malloc("socket addr array", maxsockets, sizeof(struct s_fd_sockbuf*));
    assert( g_ff_socket_vector );
    
    stklist_init(&g_ff_socket_ctl, maxsockets);
    for(i=maxsockets-1; i>0; i--){
        stklist_push(&g_ff_socket_ctl, i);
    }

/*******
    g_ff_in_nb_vec = ( uint64_t *) rte_malloc("socket in_nb vec", maxsockets, sizeof(uint64_t));
    assert( g_ff_in_nb_vec );
    g_ff_out_nb_vec = ( uint64_t *) rte_malloc("socket out_nb vec", maxsockets, sizeof(uint64_t));
    assert(g_ff_out_nb_vec);
*******/

    return 0;
}

/*****************************************************************************************************************
 * each f-stack core has one up_ring/down_ring
 * each app socket worker has sockop_cache/wndmsg_cache.
 * input param: procid
 * **************************************************************************************************************/
int ff_init_socket_info(int i){
    char	tmp_name[RTE_RING_NAMESIZE];
    int     is_primary = (rte_eal_process_type() == RTE_PROC_PRIMARY)? 1: 0;
 
    snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "ff_up_wnd_r_", i);
    g_fstk_ctl[i].ff_up_wnd_r = create_ring(tmp_name, APP_RING_SIZE, lcore_conf.socket_id, 0 );

    //snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "ff_up_ctl_r_", i);
    //g_fstk_ctl[i].ff_up_ctl_r  = create_ring(tmp_name, APP_OP_RING_SIZE, lcore_conf.socket_id, 0 );
    InitDgramSock(&g_fstk_ctl[i].ff_up_ctl_us, ff_ux_sockf, 1, 0);

    snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "ff_down_wnd_r_", i);
    g_fstk_ctl[i].ff_down_wnd_r  = create_ring(tmp_name, APP_RING_SIZE, lcore_conf.socket_id, 0 );

    snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "ff_down_ctl_r_", i);
    g_fstk_ctl[i].ff_down_ctl_r  = create_ring(tmp_name, APP_OP_RING_SIZE, lcore_conf.socket_id, 0 );

    snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "sockop_pool_", i);
    if ( is_primary )
        g_fstk_ctl[i].ff_ctlmsg_pool = rte_mempool_create(tmp_name, CTLPOOL_SIZE,
                                                    sizeof(struct s_ff_ctl_msg),
                                                    CTL_MSGPOOL_CACHE_SIZE, 0,
                                                    NULL, NULL,
                                                    NULL, NULL,
                                                    SOCKET_ID_ANY, 0);
    else
        g_fstk_ctl[i].ff_ctlmsg_pool = rte_mempool_lookup(tmp_name);
    
    if ( g_fstk_ctl[i].ff_ctlmsg_pool == NULL){
        rte_panic("create pool::%s failed, %s!\n", tmp_name, rte_strerror(rte_errno));
        return -1;
    }

    snprintf(tmp_name, RTE_RING_NAMESIZE, "%s%u", "wndmsg_pool_", i);
    if ( is_primary )
        g_fstk_ctl[i].ff_wndmsg_pool = rte_mempool_create(tmp_name, MSGPOOL_SIZE,
                                                    sizeof(struct s_ff_wnd_msg),
                                                    WND_MSGPOOL_CACHE_SIZE, 0,
                                                    NULL, NULL,
                                                    NULL, NULL,
                                                    SOCKET_ID_ANY, 0);
    else
        g_fstk_ctl[i].ff_wndmsg_pool = rte_mempool_lookup(tmp_name);
    if ( g_fstk_ctl[i].ff_wndmsg_pool == NULL){
        rte_panic("create pool::%s failed, %s!\n", tmp_name, rte_strerror(rte_errno));
        return -1;
    }

	ff_init_fd_ctl(g_fd_total);
    return 0;
}

/**************************************************************************
 * create app caches for each socket worker
 * called by app.
 * ***********************************************************************/
int ff_creat_app_info( uint16_t appid )
{
    struct epoll_event ev = {0};
    
    //RTE_ASSERT( appid < FF_MAX_SOCKET_WORKER );
    g_app_ctl[appid].app_ctlmsg_cache = rte_mempool_cache_create(CTL_MSGPOOL_CACHE_SIZE, SOCKET_ID_ANY);
    g_app_ctl[appid].app_wndmsg_cache = rte_mempool_cache_create(WND_MSGPOOL_CACHE_SIZE, SOCKET_ID_ANY);

    g_app_ctl[appid].evt_fd = eventfd(0, EFD_CLOEXEC|EFD_NONBLOCK);
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

	InitDgramSock(&g_app_ctl[appid].app_up_ctl_us, app_ux_sockf, 1, 0);
/************
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
*************/
    return 0;
}

/********************************************************************************
 * get new app worker id, which is identical for each worker thread.
 * 
********************************************************************************/
int ff_get_newid(){
    return 0;
}

/***************************************
 * get msg for communicate between fstack and socket worker.
 * In this function, up msg was allocated in fstack; down msg was allocated in app.
****************************************/
void* ff_get_sock_msg(int16_t type, uint16_t procid ){
    struct ff_fstk_ctl* p_fstk = &g_fstk_ctl[procid];
    struct ff_app_ctl*  p_app = &g_app_ctl[procid];
    void* ptr = NULL;

    switch ( type ){
    case UP_CTL_MSG:
        rte_mempool_get(p_fstk->ff_ctlmsg_pool, &ptr);
        break;
    case UP_WND_MSG:
        rte_mempool_get(p_fstk->ff_wndmsg_pool, &ptr);
        if ( unlikely( NULL==ptr ) ){
            RTE_LOG(INFO, USER1, "ff_get_sock_msg get failed, wndmsg mempool freecout %d.", 
                    rte_mempool_avail_count(p_fstk->ff_wndmsg_pool) );
        }
        break;
    case DOWN_CTL_MSG:
        rte_mempool_generic_get(p_fstk->ff_ctlmsg_pool, &ptr, 1, p_app->app_ctlmsg_cache);
        break;
    case DOWN_WND_MSG:
        rte_mempool_generic_get(p_fstk->ff_wndmsg_pool, &ptr, 1, p_app->app_wndmsg_cache);
        break;
    default:
        ptr = NULL;
        break;
    }
    return ptr;
}

/***************************************
 * free msg for communicate between fstack and socket worker.
 * In this function, up msg was freed in app; down msg was freed in fstack.
****************************************/
int ff_free_sock_msg( int16_t type, uint16_t procid, void** obj, uint32_t num){
    struct ff_fstk_ctl* p_fstk = &g_fstk_ctl[procid];
    struct ff_app_ctl*  p_app = &g_app_ctl[procid];
    if ( !num ) return -1;
    switch ( type ){
    case UP_CTL_MSG:
        rte_mempool_generic_put(p_fstk->ff_ctlmsg_pool, obj, num, p_app->app_ctlmsg_cache);
        break;
    case UP_WND_MSG:
        rte_mempool_generic_put(p_fstk->ff_wndmsg_pool, obj, num, p_app->app_wndmsg_cache);
        break;
    case DOWN_CTL_MSG:
        rte_mempool_put_bulk(p_fstk->ff_ctlmsg_pool, obj, num);
        break;
    case DOWN_WND_MSG:
        rte_mempool_put_bulk(p_fstk->ff_wndmsg_pool, obj, num);
        break;
    default:
        return -1;
        break;
    }
    return 0;
}

static inline void ff_vec_setfd(int sockfd ){
    g_ff_socket_vector[sockfd]->sock_fd = sockfd;
}
static inline void ff_vec_setid(int sockfd, int appid ){
    g_ff_socket_vector[sockfd]->app_id = appid;
}
/**************************************************************************
 * set fdsock->so  =  i_so.
 * called as soon as socreate().
 * ************************************************************************/
static inline void ff_vec_setso(int sockfd, void* i_so ){
    g_ff_socket_vector[sockfd]->so = i_so;
}

void* ff_get_fdsock( int s ){
    return (void*)g_ff_socket_vector[s];
}
/**************************************************************************
 * fstack get struct socket from g_ff_socket_vector indexed by fd.
 * 
 * In 
 * ************************************************************************/
void* ff_get_sock_obj( int s ){
    return (void*)g_ff_socket_vector[s]->so;
}

int ff_get_sockst(int s){
	return g_ff_socket_vector[s]->so_stat;
}
int ff_set_sostat(int s, int st){
	g_ff_socket_vector[s]->so_stat |= st;
	return g_ff_socket_vector[s]->so_stat;
}
#define FF_EPOLL_IN		0x01
#define FF_SO_USING		0x02
#define FF_EPOLL_OUT	0x04
#define FF_SO_ESTAB		0x08
#define FF_SO_FIN    	0x10
#define FF_SO_RESET		0x20

static void ff_clear_epollin(s){
	g_ff_socket_vector[s]->so_stat &= (~FF_EPOLL_IN);
}
void ff_clear_epollout(s){
	g_ff_socket_vector[s]->so_stat &= (~FF_EPOLL_OUT);
}
int ff_get_err(int fd){
	return (int)g_ff_socket_vector[fd]->so_error;
}

static inline int32_t ff_can_rx(int s){

	return g_ff_socket_vector[s]->app_copied_nb < g_ff_socket_vector[s]->stk_input_nb;
	
}

/**************************************************************************
 * fstack get struct socket from g_ff_socket_vector indexed by fd.
 * 
 * In 
 * ************************************************************************/

/**************************************************************************
 * used by stack to increase the stk_input_nb.
 * In rx buff, tcp_input incread the stk_input_nb.
 * In tx buff, tcp_output increase the stk_input_nb.
 * ************************************************************************/
void ff_stknb_inc(void* p, int nb){
    struct s_fd_sockbuf* p_fdsock = p;
    p_fdsock->stk_input_nb += nb;
}
/**************************************************************************
 * used by stack to set socket error.
 * 
 * 
 * ************************************************************************/
void ff_soerr_set(void* p, uint16_t err){
    struct s_fd_sockbuf* p_fdsock = p;
    p_fdsock->so_error = err;
}

void ff_set_offset(void* p, uint16_t nb){
    struct s_fd_sockbuf* p_fdsock = p;
    p_fdsock->m_offset = nb;
}

/**************************************************************************
 * stack set the sb_app_recv to the start position of socket buffer.
 * In rx, tcp_input set sb_app_recv to sb_mb when sb_app_recv==NULL.
 * p ---> s_fd_sockbuf
 * pos----> struct mbuf* in sockbuf->sb_mb link.
 * ************************************************************************/
void* ff_get_rcvptr( void* p ){
	struct s_fd_sockbuf* p_fdsock = p;
	return (void*)p_fdsock->sb_app_recv;
}
void ff_set_rcvptr( void* p, void* m ){
	struct s_fd_sockbuf* p_fdsock = p;
	p_fdsock->sb_app_recv = (uint64_t)m;
}

void ff_epnb_inc(int s){
    struct s_fd_sockbuf* p_fdsock = g_ff_socket_vector[s];
    p_fdsock->epoll_nb++;
}

/************************************************
 * in parameter is struct s_fd_sockbuf *
 * rx mbuf is_empty means all data has been copy out by app.
 * tx_mbuf is_empty means all data has been send out by fstack.
 * *********************************************/
int ifsock_empty(void* p){
    struct s_fd_sockbuf* p_sockinfo = p;
	return p_sockinfo->app_copied_nb == p_sockinfo->stk_input_nb? 1 : 0;
}

/************************************************
 * app copy data from some complete mbuf in rxbuffer.
 * sb_app_recv is the next mbuf that app will copied from.
 * refer to soreceive_stream
 * *********************************************/
int ff_read_rx( int fd, void *buf, size_t len ){
    int32_t o_len = 0, firstmsg=0;
    struct mbuf    *mb = NULL;
    struct s_fd_sockbuf* ptr = NULL;

    if ( fd >= g_fd_total ){
    	ff_errno = ff_EBADF;
        return -1;
    }
    if ( !ff_can_rx(fd) ){
    	ff_errno = ff_EAGAIN;
    	return 0;
    }
    ptr = g_ff_socket_vector[fd];
    if ( unlikely(ptr->sb_app_recv == 0) ){
        /* set sb_app_recv to sb_mb */
        ptr->sb_app_recv = (u_int64_t)ff_get_rcvbuf_start( ptr->so );
        ptr->m_offset = 0;
    }
    if ( unlikely(ptr->sb_app_recv == 0) ){
        /*  the sock buf is empty.  */
        ff_clear_epollin(fd);
        ff_errno = ff_EAGAIN;
        return -1;
    }
    //firstmsg = ( g_ff_socket_vector[fd]->app_copied_nb == 0 )? 1 : 0;
    o_len = ff_sorxcopy( ptr->so, (void*)ptr->sb_app_recv, buf, len, ptr->m_offset);
    if ( o_len < 0 ){           // error
    	ff_clear_epollin(fd);
        return -1;
    }
    else if ( o_len ==0 )       // fin
    {
        ff_clear_epollin(fd);
        return 0;
    }
    
    ptr->app_copied_nb += o_len;
    if ( ptr->app_copied_nb == ptr->stk_input_nb ){
        // recv buf is empty.
        ff_clear_epollin(fd);
    }
    else if ( ptr->app_copied_nb < ptr->stk_input_nb  ){
    	CList_Push(&g_app_ctl[this_appid].fd_list, fd);
    }
    return o_len;
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
    if ( unlikely( ret!=0 ) ){
        ff_free_sock_msg(UP_CTL_MSG, this_procid, (void**)&msg, 1);
        return -1;
    }
    if ( likely( 1 == rte_ring_count(up_ring) ) ){
        eventfd_write(g_app_ctl[this_appid].evt_fd, 1);
    }
    return 0;
}

/******************************************************************************
 * fstack recv SOCKET_EV, create new socket.
 * get socket from global vector and allocate new struct socket.
 * ***************************************************************************/
int ff_new_socket(int domain, int type, int protocol, int appid){
    int32_t fd = 0;
    void* so = NULL;

    fd = (int32_t)stklist_pop(&g_ff_socket_ctl);
    if ( unlikely( g_ff_socket_vector[fd]==NULL) ){
        so = ff_socreate(domain, type, protocol );
        if ( unlikely(so == NULL) ){
            stklist_push(&g_ff_socket_ctl, fd);
            return -1;
        }
        g_ff_socket_vector[fd] = rte_zmalloc("socketinfo", sizeof(struct s_fd_sockbuf), RTE_CACHE_LINE_SIZE);
        assert( NULL!= g_ff_socket_vector[fd] );

        ff_vec_setfd( fd );
        ff_vec_setid( fd, appid );
        ff_vec_setso( fd, so );
        *(ff_get_rxbuf_fdinfo(so)) = g_ff_socket_vector[fd];
        *(ff_get_txbuf_fdinfo(so)) = g_ff_socket_vector[fd];        
    }    

    return fd;
}

int ff_chk_sockfd(int fd){
    return g_ff_socket_vector[fd]==NULL? 0 : ( g_ff_socket_vector[fd]->sock_fd <= 0? 0 : 1 );
}

/*****************************************************
 * fstack recv socket open event, get new fd, 
 * update the msg, sent it back. no need allocating new s_ff_ctl_msg.
 * **************************************************/
static int ff_sock_proc(struct s_ff_ctl_msg* sock_req){
    int fd = 0;
    struct s_ff_ctl_msg* rsp = NULL;
    fd = ff_new_socket(sock_req->req.sock_req.domain, sock_req->req.sock_req.type, 
                        sock_req->req.sock_req.protocol, sock_req->app_id);
    if ( unlikely(fd <= 0) ){
        return -1;
    }

    rsp = (struct s_ff_ctl_msg*)sock_req; // reuse sock request msg.
    rsp->sock_fd = rsp->rsp.op_rsp.result = fd;
    if (unlikely(FF_SENDUP_CTLMSG((char*)&rsp, sizeof(rsp)) != sizeof(rsp)) ){
        RTE_LOG(INFO, USER1, "ff_sock_ev_proc enqueue failed." );
        return -1;
    }
    ff_set_sostat(fd, FF_SO_USING);             //  this socket is using.

    return 0;
}

/*****************************************************
 * fstack do connect action.
 * rsp message will be sent after connectting completed.
 * 
 * **************************************************/
static int ff_conn_proc(struct s_ff_ctl_msg* req){
    if (ff_get_sock_obj( req->sock_fd ) == NULL){
        RTE_LOG(INFO, USER1, "ff_conn_proc invalid fd %d.", req->sock_fd );
        return -1;
    }
    ff_soconnect(ff_get_sock_obj(req->sock_fd), (void*)&req->req.conn_req.p_addr, req->req.conn_req.len );
    ff_free_sock_msg(DOWN_CTL_MSG, this_procid, (void**)&req, 1);
    return 0;
}

/*****************************************************
 * fstack send connect rsp to app after connect op completed.
 * should be called by tcp_input routing when connect OK, connect failed, or timeout.
 * 
 * **************************************************/
int ff_conn_ok_sync(int fd, int res){
    struct s_ff_ctl_msg* rsp = ff_get_sock_msg( UP_CTL_MSG, this_procid);

    rsp->app_id = this_procid;
    rsp->event = CONNECT_EV;
    rsp->sock_fd = fd;
    rsp->rsp.op_rsp.result = 0;
    if ( unlikely( FF_SENDUP_CTLMSG(rsp, sizeof(struct s_ff_ctl_msg)) != 1)){
        ff_free_sock_msg(UP_CTL_MSG, this_procid, (void**)&rsp, 1);
        RTE_LOG(INFO, USER1, "ff_connect_ok enqueue failed." );
        return -1;
    }

    return 0;
}

int ff_conn_ok_async(int fd, int res){
    struct s_ff_wnd_msg* rsp = ff_get_sock_msg( UP_WND_MSG, this_procid);

    rsp->app_id = this_procid;
    rsp->event = FF_RX_WND_EV|FF_TX_WND_EV;
    rsp->sock_fd = fd;
    rsp->mod_nb = 0;
    if ( unlikely( FF_SENDUP_WNDMSG(rsp) != 1)){
        ff_free_sock_msg(UP_WND_MSG, this_procid, (void**)&rsp, 1);
        RTE_LOG(INFO, USER1, "ff_connect_ok enqueue failed." );
        return -1;
    }

    return 0;
}

static inline int ff_close_proc(struct s_ff_ctl_msg* req){
    void* so = NULL;
    
    assert(NULL!=req);
    so = ff_get_sock_obj( req->sock_fd );
    if ( so  == NULL){
        RTE_LOG(INFO, USER1, "ff_close_proc invalid fd %d.", req->sock_fd );
        return -1;
    }
    ff_soclose(so);
    ff_free_sock_msg(DOWN_CTL_MSG, this_procid, (void**)&req, 1);
	return 0;
}

/**************************************************
 * fstack dequeue req from down ctl ring, and proc the req.
 * called by fstack's main circle.
 * ***********************************************/
int ff_proc_ctlreq_msg(struct s_ff_ctl_msg* req){
    switch(req->event){
        case SOCKET_EV:
            ff_sock_proc( (struct s_ff_ctl_msg*) req);
            break;
        case CONNECT_EV:
            ff_conn_proc( (struct s_ff_ctl_msg*) req);
            break;
        case CLOSE_EV:
        	ff_close_proc((struct s_ff_ctl_msg*) req);
        	break;
        //case BIND_EV:
            //break;
        default:
            break;
    };
    return 0;
}

/************************************************************
 * fstack update all the rx buffer.
 * not support  PR_ADDR/MSG_PEEK/MT_CONTROL/MSG_SOCALLBCK/MSG_WAITALL, recv with flags = 0.
 * 
 * ********************************************************/
int ff_rxwnd_update( int sock_fd, int len ){
    int error = 0;
    void* so;
    so = ff_get_sock_obj(sock_fd);
    if ( so==NULL ){
        return -1;
    }
    
    ff_tcp_rxupdate( so, len, (void*)g_ff_socket_vector[sock_fd]->sb_app_recv);
	return 0;
}
/***************************************
* fstack update txbuffer after app send something, using wndmsg carry mbuf is secure.
* refer to sosend_generic -- copy user data into mbuf
* refer to tcp_usr_send -- sbappendstream_locked(&so->so_snd, m, flags);
****************************************/
int ff_txwnd_update( int sock_fd, uint32_t mod_nb ){
    int error = 0;
    int len = mod_nb;
    void* so = ff_get_sock_obj(sock_fd);

	//sbappendstream_locked(&so->so_snd, m, flags);
	return 0;
}

static inline int ff_wndmsg_proc(int fd, uint16_t ev, int mod){
    return ( ev==RX_WND_EV? ff_rxwnd_update(fd, mod ):ff_txwnd_update(fd,mod));
}

/*********************************************************
 * Process the down wnd msg from app to f-stack.
 * Try to update same socket once when continuous same socket's wnd msg. Is it really needed?
 * ----- Maybe should burst one batch, divied all msg, process all socket at the end, each socket was processed only once.
 * ******************************************************/
void ff_proc_wnd_ring(){
	struct s_ff_wnd_msg* ele_burst[MAX_DOWN_RING_BURST];
	int i = 0, nb_rx = 0, count = 0;
    int32_t wnd_mod = 0;
    struct s_ff_wnd_msg* p_msg = NULL;

    if ( unlikely( rte_ring_empty(g_fstk_ctl[this_procid].ff_down_wnd_r) ) ){
        return ;
    }

	nb_rx = rte_ring_dequeue_burst(g_fstk_ctl[this_procid].ff_down_wnd_r, 
	            (void**)ele_burst, MAX_DOWN_RING_BURST, NULL);
/*****
    while ( nb_rx ){
        for ( i=0; i<nb_rx; i++){
            if( wnd_mod == 0 || !pre_msg || FF_SAME_WNDMSG(pre_msg, ele_burst[i]) ){
                wnd_mod += ele_burst[i]->mod_nb;
                pre_msg = ele_burst[i];
                continue;
            }
            if ( !FF_SAME_WNDMSG(pre_msg, ele_burst[i]) ){
                // update continous same socket wnd.
                ff_wndmsg_proc( pre_msg->sock_fd, pre_msg->event, wnd_mod );
                wnd_mod = 0;
                pre_msg = NULL;
            }
        }
        //  runs out quota once.
        count += nb_rx;
        if ( unlikely(count >= MAX_DOWN_RING_MSG ) )					
            break;
        nb_rx = rte_ring_dequeue_burst(g_fstk_ctl[this_procid].ff_down_wnd_r, (void**)ele_burst, MAX_PKT_BURST, NULL);
    }
******/
    while ( nb_rx ){
    	for ( i=0;i<nb_rx;i++ ){
    	    p_msg = ele_burst[i];
            ff_wndmsg_proc( p_msg->sock_fd, p_msg->event, p_msg->mod_nb);
        }
        ff_free_sock_msg(DOWN_WND_MSG, this_procid, (void**)ele_burst, nb_rx);
        count += nb_rx;
        if ( unlikely(count >= MAX_DOWN_RING_MSG ) )					
            break;
        nb_rx = rte_ring_dequeue_burst(g_fstk_ctl[this_procid].ff_down_wnd_r, 
                    (void**)ele_burst, MAX_PKT_BURST, NULL);
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
	nb_rx = rte_ring_dequeue_burst(g_fstk_ctl[this_procid].ff_down_ctl_r, (void**)ele_burst, MAX_DOWN_RING_BURST, NULL);
    while ( nb_rx ){
        for ( i=0; i<nb_rx; i++){
            ff_proc_ctlreq_msg( ele_burst[i] );        // need to free the msg in .
        }
        //ff_free_sock_msg(DOWN_WND_MSG, this_procid, (void**)ele_burst, nb_rx);
        count += nb_rx;
        if ( unlikely(count >= MAX_DOWN_RING_MSG ) )
            break;        
        nb_rx = rte_ring_dequeue_burst(g_fstk_ctl[this_procid].ff_down_wnd_r, (void**)ele_burst, MAX_PKT_BURST, NULL);
    }
}

enum {
	WND_UPDATE_MSG = 0,
	CONNOK_MSG,
	FIN_MSG,
	RST_MSG,
	ACCEPT_MSG
};

/******************************************************************************
 * Check whether can send rx notification. 
 * return 0: should not wakeup.
 * return 1: must wakeup.
 * ***************************************************************************/
int ff_soread_chk(){
    return 0;
}

/******************************************************************************
 * fstack wakeup the waiting queue after rx buffer has enough bytes.
 * Called by tcp_input routing, if need to wakeup waiting app.
 * Type : wnd_msg_notify  fin_notify  rst_notify
 * ***************************************************************************/
int ff_tcp_rwakeup(void* p, int type){
    int16_t mod;
    int num, len;
    struct s_fd_sockbuf* sockinfo = p;
    struct s_ff_wnd_msg* p_msg = NULL;

    if ( type == 2 ){
    	// no socket error happened.
    	if (sockinfo->stk_wakeup_nb > sockinfo->epoll_nb
    		//|| sockinfo->stk_input_nb == sockinfo->app_copied_nb 
    		|| sockinfo->so_stat&FF_EPOLL_IN
    		){
        	/* there is at least 1 wnd_msg in the up_wnd_ring. or socket has been epollin. */
        	return 0;
        }
        len = sockinfo->stk_input_nb - sockinfo->app_copied_nb;
        assert( len>0 );
    }

    p_msg = ff_get_sock_msg( UP_WND_MSG, this_procid);
    if ( unlikely(NULL==p_msg )){
        return -1;
    }
    p_msg->app_id = sockinfo->app_id;
    p_msg->event = RX_WND_EV;
    p_msg->mod_nb = len;		//  no use length, app read as much as can do.
    p_msg->sock_fd = sockinfo->sock_fd;

    if ( unlikely(FF_SENDUP_WNDMSG(p_msg) != 0)){
        ff_free_sock_msg(DOWN_WND_MSG, this_procid, (void**)&p_msg, 1);
        RTE_LOG(INFO, USER1, "ff_tcp_rwakeup enqueue failed." );        
        return -1;
    }
    sockinfo->stk_wakeup_nb ++;
    return 0;
}

#define __COMM_FUNC_
// define some common functions that can be called by up/down layer.
// this source belongs to FF_HOST_SRCS, should be compiled with fault headers, linked with system libs.
// can't be linked with bsd functions, can be linked with dpdk functions.
// using C++ stl containers may be better.
int stklist_init(StackList_t*p, int size){
    int i = 0;
    
    if (p==NULL || size<=0){
        return -1;
    }
    p->size = size;
    p->top = 0;
    if ( posix_memalign((void**)&p->ele, sizeof(uint64_t), sizeof(uint64_t)*size) != 0)
        return -2;
    
    return 0;
}
uint64_t stklist_pop(StackList_t *p)
{
    if (p==NULL)
        return (uint64_t)-1;

    if (p->top > 0 ){
        return (uint64_t)p->ele[--p->top];
    }
    else
        return (uint64_t)-1;
}

//id: the id of element to be freed.
//return code: -1: faile;  >=0:OK.
int stklist_push(StackList_t *p,  const uint64_t val){
    int tail = 0;
    
    if (p==NULL)
        return -1;
    if (p->top < p->size){
        p->ele[p->top++] = val;
        return 0;
    }
    else
        return -1;
}
int stklist_size(StackList_t * p){
    return p->size;
}

int InitDgramSock(UnixSock_t* pSock, const char* path, 
        uint32_t blocked, uint32_t bufsz)
{
    int clt_fd,ret, flag;
    socklen_t len ;
    struct sockaddr_un *SockAddr;
    int sendBufSize ;
    
    pSock->sockfd = socket(AF_UNIX,SOCK_DGRAM,0);
    if(pSock->sockfd == -1)
    {
        return -1;
    }

    unlink(path);
    SockAddr = &pSock->addr;
    memset(SockAddr,0,sizeof(struct sockaddr_un));
    SockAddr->sun_family = AF_UNIX ;
    memset(SockAddr->sun_path, 0, sizeof(SockAddr->sun_path));
    strncpy(SockAddr->sun_path, path, sizeof(SockAddr->sun_path)-1);
    ret = bind(pSock->sockfd, (struct sockaddr*)SockAddr, 
                sizeof(struct sockaddr_un));
    if(ret == -1)
    {
        close(pSock->sockfd);
        return -2;
    }
    if ( !blocked ){
        flag = fcntl(pSock->sockfd, F_GETFL, 0);
        if (fcntl(pSock->sockfd, F_SETFL, flag | O_NONBLOCK) == -1){
            close(pSock->sockfd);
            return -3;
        }
    }
    
    if ( bufsz > 0 ){
        len=sizeof(sendBufSize);
        sendBufSize = bufsz ;
        ret=setsockopt(pSock->sockfd,SOL_SOCKET,SO_SNDBUF,&sendBufSize,len);
        if(ret==-1){
            close(pSock->sockfd);
            return -4;
        }
    }

    return 0;  
}
void InitUnixAddr(struct sockaddr_un *addr, const char* path, unsigned int len)
{
	if (addr==NULL || len >= sizeof(addr->sun_path))
	{
		return;
	}
	addr->sun_family = AF_UNIX ;
    strncpy(addr->sun_path, path, sizeof(addr->sun_path));
    addr->sun_path[sizeof(addr->sun_path)-1] = 0;
}

int UxSktSend(int sockfd, struct sockaddr_un* remote, char* data, unsigned int datalen)
{
	int sendsize = 0;
	int len = 0;

	if (sockfd<=0 || remote==NULL)
	{
		return -1;
	}
	len = sizeof(struct sockaddr_un);
	sendsize = sendto(sockfd, data, datalen, 0, (struct sockaddr*)remote, len);
	/*
    if (sendsize < 0 )
	{
		SleepUs(5);
		sendsize = sendto(sockfd, data, datalen, 0, (struct sockaddr*)remote, len);
	}
    */
	return sendsize;
}

int UxSktRecv(int sockfd, struct sockaddr_un* remote, char* data, unsigned int datalen)
{
	int recvsize = 0;
	int len = 0;

	if (sockfd<=0 || remote==NULL )
	{
		return -1;
	}
	len = sizeof(struct sockaddr_un);
	recvsize = recvfrom(sockfd, data, datalen, 0, (struct sockaddr*)remote, (socklen_t*)&len);
	if (recvsize == -1)
	{
		if (errno != EINTR && errno != ECONNABORTED && errno != EAGAIN && errno != ENOBUFS && errno != EWOULDBLOCK && errno != EPROTO)
		{
			return -2;
		}
	}
	return recvsize;
}

static int cal_exp (unsigned long size)
{
	unsigned long i = 1;
	int j = 0;

	while(i < size) {
		i *= 2;
		j ++;
	}
	return (i);
}

/**********************
 ����ѭ�����е�ʵ�ʿռ䳤�ȣ� 
 ʵ�ʿռ�Ӧ���ǲ�С�� ���size����С 2�ݡ�
**********************/
int CList_init(CList_t *p, int size)
{
	int i = 0;
	int len = 0;
	
	if (p==NULL || size<=0)
	{
		return -1;
	}
	len = cal_exp(size);
	//p->ele = (int*)malloc(sizeof(int)*size);
	posix_memalign((void**)&p->ele, BITS_SIZE, size*sizeof(long));
	if (p->ele == NULL)
		return -2;
	
	p->Head = p->Tail = 0;
	p->size = len;
	
	return p->size;
}

//  ��ȡѭ�����еĿ�����Դ����
int CList_GetLen(CList_t *p)
{
	int head, tail;
	
	if(p==NULL)
		return -1;
	head = p->Head;
	tail = p->Tail;
	return  ((tail - head + p->size) & (p->size -1));
}

int CList_IsEmpty(CList_t *p)
{
	return  p->Head == p->Tail;
}

int CList_IsFull(CList_t *p)
{
	return  p->Head == ((p->Tail + 1) & (p->size -1)) ;
}

//return code: -1: faile;  >=0:OK.
int CList_Pop(CList_t *p)
{
	int head = 0;
	
	if(p==NULL)
		return -1;

	if ( !CList_IsEmpty(p))
	{
		head = p->Head;		
		p->Head = (p->Head + 1) & (p->size-1);
		return p->ele[head];
	}
	else
		return -1;
}

//id: the id of element to be push into list.
//return code: -1: faile;  >=0:OK.
int CList_Push(CList_t *p, long val)
{
	int tail = 0;
	
	if(p==NULL)
		return -1;
	if ( !CList_IsFull(p) )
	{
		tail = p->Tail;
		p->ele[tail] = val;
		p->Tail = (p->Tail + 1) & (p->size-1);
		
		return 0;
	}
	else
		return -1;
}

inline int CList_GetSize(CList_t *p)
{
	if(p==NULL)
		return -1;
	return p->size;
}




