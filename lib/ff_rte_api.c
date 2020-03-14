/********************
 * ret_mbuf api  functions, included in FF_HOST_SRCS.
 * this defined the base func for bsb/mbuf.
 * *****************/
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

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
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "ff_config.h"
#include "ff_dpdk_if.h"
#include "ff_veth.h"
#include "ff_rte_api.h"

#define MEMPOOL_CACHE_SIZE      64
#define FF_PRIVAT_SIZE          0 
#define FF_RTE_MBUF_BUF_SIZE    (RTE_PKTMBUF_HEADROOM+0) 

/***************************************
 * this way saves the BSD's mbuf in rte_mbuf's headroom, keep mbuf's integrity.
 * when send: alloc rte_mbuf, pretend mbuf in headroom;
 * when recv: pretend mbuf in headroom of rte_mbuf which is allocated in recving.
 * RTE_PKTMBUF_HEADROOM set to 256,  data size was set to 0,  then all 256B is enough to save BSD mbuf.
 * 
 * ************************************/
/**********************************************
 * | rte_mbuf | HEADROOM         256B      | 64 B   |
 * | rte_mbuf | BSD mbuf 136B       |     data      | 
 * ********************************************/
#define MBUF_2_RTE_MBUF(m) (struct rte_mbuf*)( ((void*)m) - sizeof(struct rte_mbuf)  )

static struct rte_mempool*	g_bsdmbufbuf_pool = NULL;
static int numa = 0;

/****************
 * each rte_mbuf_ext_shared_info attached to one ext data block.
 * After data block sented, ext_shared_info's free_cb will be called.
 * 
 * ***************/
static struct rte_mbuf_ext_shared_info g_shd_info;

static inline void ext_mem_free(void *addr __rte_unused, void *opaque)
{
    //printf( "ext_mem_free\n" );

    return ;
}

inline struct rte_mbuf* ff_mbuf2_rtembuf( void* m )
{
    return (struct rte_mbuf*)( ((void*)m) - sizeof(struct rte_mbuf) );
}

int ff_mbuf_pool_init(const char* app_name)
{
	int socketid = 0, lcore_id = 0;
	int numa_on;
    char    s[32];

    numa_on = ff_global_cfg.dpdk.numa_on;
	if (numa_on) {
        socketid = rte_lcore_to_socket_id(rte_lcore_id());
    }
    if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
        
        /*snprintf(s, sizeof(s), "%s_hdr_%d", app_name, socketid);
        g_bsdmbufhdr_pool = rte_pktmbuf_pool_create(s, Hdr_Mbuf_Num,
                    MEMPOOL_CACHE_SIZE, 0,
                    FF_RTE_MBUF_BUF_SIZE, socketid);
        */         
        snprintf(s, sizeof(s), "%s_ext_%d", app_name, socketid);           
        g_bsdmbufbuf_pool = rte_pktmbuf_pool_create(s, EXT_Mbuf_Num,
                    MEMPOOL_CACHE_SIZE, 0,
                    FF_RTE_MBUF_BUF_SIZE, socketid);
        
    } else {
        //snprintf(s, sizeof(s), "%s_hdr_%d", app_name, socketid);
        //g_bsdmbufhdr_pool = rte_mempool_lookup(s);        
		snprintf(s, sizeof(s), "%s_ext_%d", app_name, socketid);
		g_bsdmbufbuf_pool = rte_mempool_lookup(s);
    }
    if ( !g_bsdmbufbuf_pool){
        printf("create %s mbuf pool on socket %d failed.\n", app_name, socketid);
        return -1;
    }

    g_shd_info.fcb_opaque = NULL;
    g_shd_info.free_cb = ext_mem_free;
    // rte_pktmbuf_detach_extbuf will decrease this cnt， if cnt==0， free_cb will be called.
    rte_mbuf_ext_refcnt_set(&g_shd_info, 1);
    
    return 0;
}

// alloc new rte_mbuf and return  BSD mbuf ptr.
// void* ff_pkt_new(int flags, void** p)
void* ff_pkt_new(int flags) //, void** p)
{
    struct rte_mbuf*        m_dpdk = NULL;

    m_dpdk = rte_pktmbuf_alloc( g_bsdmbufbuf_pool );
	if ( unlikely(!m_dpdk) )
	{
		printf("ff_mbuf_new failed.\n");
		return NULL;
	}
    rte_pktmbuf_reset(m_dpdk);
    rte_pktmbuf_prepend(m_dpdk, RTE_PKTMBUF_HEADROOM);
	return rte_pktmbuf_mtod(m_dpdk, void*);
}

// get buf_addr from rte_mbuf, BSD mbuf inserted in buf_addr.
void* ff_pkt_getbuf( void* pkt )
{
    return ((struct rte_mbuf*)pkt)->buf_addr;
}

void ff_pkt_attach_extbuf( void* m , void* buf, int len)
{    
	struct rte_mbuf*  m_dpdk = MBUF_2_RTE_MBUF(m);
	rte_pktmbuf_attach_extbuf(m_dpdk, buf, rte_mem_virt2iova(buf), len, &g_shd_info);
	m_dpdk->data_len = len;
}

/*************************************
 * this is attach_extbuf function in DPDK.
 * ***********************************
static inline void
rte_pktmbuf_attach_extbuf(struct rte_mbuf *m, void *buf_addr,
	rte_iova_t buf_iova, uint16_t buf_len,
	struct rte_mbuf_ext_shared_info *shinfo)
{

	RTE_ASSERT(RTE_MBUF_DIRECT(m) && rte_mbuf_refcnt_read(m) == 1);
	RTE_ASSERT(shinfo->free_cb != NULL);

	m->buf_addr = buf_addr;
	m->buf_iova = buf_iova;
	m->buf_len = buf_len;

	m->data_len = 0;
	m->data_off = 0;

	m->ol_flags |= EXT_ATTACHED_MBUF;
	m->shinfo = shinfo;
}
***************************************/
/*************************************
 * An attach_extdata function, according to rte_pktmbuf_attach_extbuf in DPDK.
 * The caller must deal the release of the ext buffer.
 * ************************************/
void ff_pkt_attach_extdata( void* m , void* buf_addr, int buf_len)
{
    struct rte_mbuf*  m_dpdk = MBUF_2_RTE_MBUF(m);

    m_dpdk->buf_addr = buf_addr;
	m_dpdk->buf_iova = rte_mem_virt2iova(buf_addr);
	m_dpdk->buf_len = m_dpdk->data_len = buf_len;
	m_dpdk->data_off = 0;
    m_dpdk->ol_flags |= EXT_ATTACHED_MBUF;
}

/********************************
 * sync rte_mbuf's pkt_len/data_len to mheader's pkt_len/m_len.
 * mheader as:
 * |  rte_mbuf   | mbuf  -------mdata(m_len)---|
 * ^                            ^
 * |                            |
 * m                           m_data 
 * ******************************************
 * this function is not good, should use dpdk's api to update rte_mbuf.
 * ******************************/
void* ff_pkt_sync_headr( void* m, void* m_data, uint16_t m_len, uint32_t pkt_len, uint32_t segs)
{
	struct rte_mbuf*  m_dpdk = MBUF_2_RTE_MBUF(m);
    m_dpdk->data_off = m_data - m;
	m_dpdk->data_len = m_len;
    m_dpdk->pkt_len =  pkt_len;
    m_dpdk->nb_segs = segs;
    return m_dpdk;
}

void ff_pkt_free(void* m)
{
    rte_pktmbuf_free(MBUF_2_RTE_MBUF(m));
}

void ff_pkt_setnext(void* prev, void* m)
{
    struct rte_mbuf* m_prev = MBUF_2_RTE_MBUF(prev);
    struct rte_mbuf* m_ptr = MBUF_2_RTE_MBUF(m);
    m_prev->next = m_ptr;
}

void ff_pkt_setlen(void* m, void* mdata, int mlen, int pktlen)
{
    struct rte_mbuf* m_dpdk = MBUF_2_RTE_MBUF(m);
    m_dpdk->data_len = mlen;
    m_dpdk->pkt_len = (pktlen > 0)?  pktlen: mlen;
    if ( RTE_MBUF_DIRECT(m_dpdk) )
    {
        // header's len had been updated in ip/ethernet, should be update here.
        // dpdk's dataoff = m->mdata - m
        m_dpdk->data_off = mdata - m;
    }
}

void* ff_pkt_sethdr( void* m, unsigned int segs )
{
    struct rte_mbuf* m_dpdk = MBUF_2_RTE_MBUF(m);
    m_dpdk->nb_segs = segs;

    return m_dpdk;
}



