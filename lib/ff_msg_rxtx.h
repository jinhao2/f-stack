/******************************************************
 * define msg between fstack and app user, using system libs and dpdk libs.
 * belongs to FF_HOST_SRCS, can not be linked with bsd source codes.
 * ****************************************************/
#ifndef FF_MSG_RXTX_
#define FF_MSG_RXTX_

typedef struct _list_pool_s
{
	long	*ele;		// ±£´æ×ÊÔ´µÄµØÖ·¿Õ¼ä
	int		size;		// ×ÊÔ´Êý×é³¤¶È
	//int		FreeNum;	// ¿ÕÏÐÊý×é³¤¶È
	int 	Head;		// Ö¸ÏòµÚÒ»¸ö¿ÕÏÐ×ÊÔ´
	int		Tail;		// Ö¸ÏòµÚÒ»¸ö·Ç¿ÕÏÐ×ÊÔ´
}CycleList_t, CList_t;

#define BITS_SIZE	sizeof(long)					//  ×Ö³¤¶È
int CList_init(CList_t *p, int size);
int CList_GetLen(CList_t *p);
int CList_IsEmpty(CList_t *p);
int CList_IsFull(CList_t *p);
int CList_Pop(CList_t *p);
int CList_Push(CList_t *p, long val);

/****************************************
unix socket ï¿½ï¿½ï¿½Æ¹Üµï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Ð­ï¿½ï¿½Õ»ï¿½ï¿½ï¿½ï¿½socketÒªï¿½ì£»
unix socket stream  dgramï¿½ï¿½ï¿½Ç¿É¿ï¿½ï¿½Ä£ï¿½ï¿½ï¿½ï¿½á¶ªï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½
unix socket ï¿½ï¿½ï¿½Õ»ï¿½ï¿½ï¿½ï¿½ï¿½Ð§ï¿½ï¿½ï¿½ï¿½ï¿½Í»ï¿½ï¿½ï¿½ï¿½Ê¾ï¿½ï¿½ï¿½Ô·ï¿½ï¿½Íµï¿½ï¿½ï¿½ï¿½Ý£ï¿½
dgramï¿½ï¿½ï¿½Ð±ß½ï¿½Ä£ï¿½streamÃ»ï¿½Ð±ß½ç£»
dgramï¿½ï¿½ï¿½Í»ï¿½Ê§ï¿½Ü¡ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Ê¾ï¿½ï¿½ï¿½Í»ï¿½ï¿½ï¿½ï¿½Þ·ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½udpï¿½ï¿½ï¿½Ä£ï¿½
net.unix.max_dgram_qlenï¿½ï¿½ï¿½Æµï¿½ï¿½ï¿½unix socketï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½dgramï¿½ï¿½ï¿½ï¿½Ä¬ï¿½ï¿½Öµ10ï¿½ï¿½Ö»ï¿½Ü·ï¿½10ï¿½ï¿½ï¿½ï¿½Ï¢ï¿½ï¿½
struct sockaddr_un shoud be separated from bsd codes.
*****************************************/
typedef struct _UNIXSOCKET_
{
	int sockfd;
	struct sockaddr_un addr;
	char	filepath[108];              // UNIX_PATH_MAX = 108 defined in <linux/un.h>
}UnixSock_t;
int InitDgramSock(UnixSock_t* pSock, const char* path, uint32_t blocked, uint32_t bufsz);
void InitUnixAddr(struct sockaddr_un *addr, const char* path, unsigned int len);
int UxSktSend(int sockfd, struct sockaddr_un* remote, char* data, unsigned int datalen);
int UxSktRecv(int sockfd, struct sockaddr_un* remote, char* data, unsigned int datalen);

#define APP_RING_SIZE           4096
#define APP_OP_RING_SIZE        128
#define APP_EPOLL_EVENTS        2
#define APP_EPOLL_TIMEOUT_MS    8

#define	FF_MAX_SOCKET_NUM		8192
#define FF_RING_BULK_NUM		2048

#define FF_MAX_SOCKET_WORKER    8
#define FF_SENDUP_SIGNAL_THRESHOLD  8
#define MAX_DOWN_RING_BURST 64
#define MAX_DOWN_RING_MSG   1024
/*********************************************************************************
* control info between fstack and app worker.
* down ctl msg ----- sync operation, app push msg into down ctl ring, fstack burst out,process and send up rsp ctl msg if needed. 
*			   Asyns operation, app app push msg into down ctl ring, fstack burst out,process and sendup wnd msg.
* down wnd msg ----- app push into down wnd ring, on the other hand fstack burst out and process.
* up ctl msg ------ fstack send to app's unix socket, send the address of the msg.
* up wnd msg ------ fstack push into up wnd ring, and signal eventfd.
**********************************************************************************/
struct ff_fstk_ctl{
    struct rte_ring*	ff_up_wnd_r;            // fstack ---> app wrker for wnd notify
    //struct rte_ring*	ff_up_ctl_r;            // fstack ---> app wrker for socket op.
    UnixSock_t          ff_up_ctl_us;           // unix socket for sync socket op, fstack ---> app wrker.
    struct rte_ring*	ff_down_wnd_r;          // app wrker ---> fstack
    struct rte_ring*	ff_down_ctl_r;          // app wrker ---> fstack, for socket op.

    struct rte_mempool* ff_wndmsg_pool;         // socket wnd msg pool.
    struct rte_mempool* ff_ctlmsg_pool;         // socket operation msg pool.
}__rte_cache_aligned;

struct ff_app_ctl{
    struct rte_mempool_cache*   app_ctlmsg_cache;           // attach to ff_sockop_pool, used by app.
    struct rte_mempool_cache*   app_wndmsg_cache;           // attach to ff_wndmsg_pool, used by app.
    int                         ep_fd;                      // epoll fd for watching evt_fd.
    int                         evt_fd;                     // eventfd used for wndmsg signal.
    UnixSock_t                  app_up_ctl_us;
    CList_t						fd_list;
    //int                         sock_epfd;
    //int                         sock_evtfd;                 // eventfd used for socket open, fstack open new fd and sent new fd in this eventfd.
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
    INIT_EV = 0,
    RX_WND_EV = 1,			//  EPOLLIN
    TX_WND_EV = 4,			//  EPOLLOUT
    SOCKET_EV = 0x1000,
    CONNECT_EV,
    CLOSE_EV,
    BIND_EV,
    LISTEN_EV,
    ACCEPT_EV,
    CLOSE_FIN_EV,
    CLOSE_RST_EV,
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
struct s_connect_req{
    struct sockaddr p_addr;
    uint16_t len;
};
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

    int32_t mod_nb;           // added bytes number.
} __rte_cache_aligned;

/*****************************************************
 *  socket info structure  for each socket buffer.
 *  socket fd <---------> struct sockbuf
 *  both app and stack worker access this structure.
 * **************************************************/
struct s_fd_sockbuf{
    int32_t             sock_fd;		            // 
    int32_t             app_id;			    /* the app that waiting for this socket. */
    uint16_t			so_error;			/* socket error */
    uint16_t			so_stat;			/* socket stat, EPOLLIN  EPOLLOUT   */
    uint16_t            m_offset;       	/* first mbuf's offset to m->data, which has been read out by app. */
    volatile u_int64_t  epoll_nb;        	/* the buffer was polled times number */

    volatile u_int64_t  sb_app_recv;	    /* app recvfrom/sendinto the mbuf sb_app_recv of sockbuf */
    volatile u_int64_t	app_copied_nb;		/* app read or write bytes number */	
    void                *so;                /* point to struct socket */
    MARKER cacheline1 __rte_cache_aligned;
    volatile u_int64_t	stk_input_nb;		/* stack input bytes number */
    volatile u_int32_t  stk_wakeup_nb;      /* stack wakeup app times number */
}__rte_cache_aligned;

extern inline void* ff_get_sock_obj( int s );
extern void* ff_get_sock_msg(int16_t type, uint16_t procid );
extern void ff_epnb_inc( int s);
extern int ff_free_sock_msg( int16_t type, uint16_t procid, void** obj, uint32_t num);
extern int ff_read_rx( int fd, void *buf, size_t len );
extern int ff_chk_sockfd(int fd);

/**************************************
 * stack list use vector to save definite objects.
 * Not Support Multithreads calling.
 * why not using stl stack.
 * ************************************/
typedef struct _list_manager_s
{
    uint64_t    *ele;        // element address, can be any pointer.
    int         size;        
    int         top;
}StackList_t;

int     stklist_init(StackList_t*p, int size);
uint64_t stklist_pop(StackList_t *p);
int     stklist_push(StackList_t * p, uint64_t val);
int 	setcpu_affinity(int core_id);

#endif

