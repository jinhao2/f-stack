/******************************************************
 * ff_msg_rxtx.h   
 * ****************************************************/
#ifndef FF_MSG_RXTX_
#define FF_MSG_RXTX_

#include "ff_dpdk_if.h"
#include "ff_config.h"
#include "ff_veth.h"
#include "ff_host_interface.h"
#include "ff_api.h"
#include "ff_memory.h"

#define APP_RING_SIZE           4096
#define APP_OP_RING_SIZE        128
#define APP_EPOLL_EVENTS        2
#define APP_EPOLL_TIMEOUT_MS    8

// control info between fstack and app worker.
struct ff_fstk_ctl{
    struct rte_ring*	ff_up_wud_r;            // fstack ---> app wrker for wnd notify
    struct rte_ring*	ff_up_ctl_r;            // fstack ---> app wrker for socket op.
    struct rte_ring*	ff_down_wnd_r;          // app wrker ---> fstack
    struct rte_ring*	ff_down_ctl_r;          // app wrker ---> fstack, for socket op.
    struct rte_mempool* ff_wndmsg_pool;         // socket wnd msg pool
    struct rte_mempool* ff_ctlmsg_pool;         // socket operation msg pool
}__rte_cache_aligned;

struct ff_app_ctl{
    struct rte_mempool_cache*   app_ctlmsg_cache;           // attach to ff_sockop_pool, used by app.
    struct rte_mempool_cache*   app_wndmsg_cache;           // attach to ff_wndmsg_pool, used by app.
    int                         ep_fd;
    int                         evt_fd;
    int                         sock_evtfd;                 // eventfd used for socket open, fstack open new fd and sent new fd in this eventfd.
    int                         sock_epfd;
}__rte_cache_aligned;

enum e_ring_type{
    CTL_MSG_RING = 0,
    WND_MSG_RING
};

enum e_msg_type{
    INIT_MSG_TYPE = 0,
    UP_CTL_MSG,
    UP_WND_MSG,
    DOWN_CTL_MSG,
    DOWN_WND_MSG
};

#define TCP_WND_THRESH_SZ   4096                // maybe configured on each socket.
/*****************************
 * each event have 2 ways, upway means from fstack to app, downway means from app to fstack.
 * event msg should be allocated from msg buffer pool.
 * **************************/
enum fstack_app_ctl_evt{
    INIT_EV = (1<<4),
    SOCKET_EV,
    CONNECT_EV,
    BIND_EV,
    LISTEN_EV,
    ACCEPT_EV,
    CLOSE_FIN_EV,
    CLOSE_RST_EV,
    RX_WND_EV,
    TX_WND_EV,
    FF_EPOLLOUT,
    ADD_EPOLLOUT_EV,
    DEL_EPOLLOUT_EV,
    DEL_EPOLLIN_EV,
    RX_OOB_MSG_EV,              // not supported.
    INVALID_EV
};

/***********************************************************
 * Fstack app wnd event same as EPOLLIN/EPOLLOUT.
 *
 * ********************************************************/ 
#define FF_RX_WND_EV EPOLLIN
#define FF_TX_WND_EV EPOLLOUT

struct s_socket_req{
    int domain;
    int type;
    int protocol;
} socket_req;

struct s_bind_req{
    struct sockaddr l_addr;
    uint16_t len;
};
#define s_connect_req s_bind_req

struct s_listen_req{
    uint16_t backlog;
};

struct s_accept_rsp{
    int32_t  newfd;
    uint16_t len;
    struct sockaddr r_addr;        // remote address
}; 

struct s_sockop_rsp{
    int32_t result;
};

struct s_ff_ctl_msg{
    uint16_t event;
    uint16_t app_id;            //app worker id  
    int32_t sock_fd;
    union{
        struct s_socket_req sock_req;
        struct s_connect_req conn_req;
        struct s_bind_req   bind_req;
        struct s_listen_req listen_req;
        struct s_accept_rsp accept_rsp;
        struct s_sockop_rsp op_rsp;
    }req,rsp;
} __rte_cache_aligned;

/*************************************
 * RWND_UPDATE_EV:
 *      up: rx buffer updated from 0 to avail_nb bytes, and app should burst as many as possible.
 *      down: app minus avail_nb bytes from rx buffer.
 * SWND_UPDATE_EV:
 *      up: tx buffer's bytes has been less than TCP_WND_THRESH_SZ, and can be add new avail_nb bytes.
 *      down: app add avail_nb bytes into tx buffer.
 * **********************************/
struct s_ff_wnd_msg{
    uint16_t event;
    uint16_t app_id;          // app worker id      
    int32_t sock_fd;

    int16_t mod_nb;           // added bytes number.
} __rte_cache_aligned;

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**************************************
 * stack list use vector to save definite objects.
 * Not Support Multithreads calling.
 * ************************************/
typedef struct _list_manager_s
{
    uint64_t    *ele;        // element address, can be any pointer.
    int         size;        
    int         top;
}StackList_t;

static inline int         stklist_init(StackList_t*p, int size);
static inline void        *stklist_pop(StackList_t *p);
static inline int         stklist_push(StackList_t * p, uint64_t val);

static int                 stklist_init(StackList_t*p, int size)
{
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

static inline stklist_pop(StackList_t *p)
{
    int head = 0;
    
    if (p==NULL)
        return NULL;

    if (p->top > 0 ){
        return (void*)p->ele[--p->top];
    }
    else
        return NULL;
}

//id: the id of element to be freed.
//return code: -1: faile;  >=0:OK.
static inline int stklist_push(StackList_t *p,  const uint64_t val){
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

static inline int stklist_size(StackList_t * p)
{
    return p->size;
}




#endif
