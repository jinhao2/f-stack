/***************************
* this file shoud be Included in FF_SRCS, compiled with freebsd files.
* Define bsd mbuf functions which should be used by BSD and ff_dpdk_if on top of rte_api.
***************************/
#include <sys/ctype.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/sched.h>
#include <sys/sockio.h>
#include <sys/uio.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
#include <net/if_tap.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <machine/atomic.h>

//#include "ff_config.h"
//#include "ff_veth.h"

#include "ff_mbuf_api.h"
#include "ff_rte_api.h"

#define FF_SET_RTEMBUF(m)    ff_pkt_setlen((void*)m, m->m_data, m->m_len, m->m_pkthdr.len)
static inline struct mbuf* ff_m_getbuf(int how, unsigned short type, int flags);
unsigned int ff_m_getsegs( struct mbuf* m );

long long g_mcnt = 0;

/******************************************
 * ff_m_getbuf() get new mbuf.
 * The mbuf can be mbuf within data.
 * The mbuf must be attached to one memory address after ff_m_getbuf() with ext flags.
 * ****************************************/
static inline struct mbuf* ff_m_getbuf(int how, unsigned short type, int flags)
{
	struct rte_mbuf*	m_dpdk = NULL;
	struct mbuf*		m_bsd  = NULL;
	int					ret    = 0;
	void*				data_p = NULL;

	m_bsd = (struct mbuf*) ff_pkt_new(flags) ;
	if ( unlikely( NULL==m_bsd ) )
	{
		printf("ff_m_getbuf: ff_pkt_new return NULL.\n");
		return NULL;
	}
	g_mcnt ++;
	ret = m_init(m_bsd, how, type, flags);
	if ( unlikely(ret!=0) )
	{
		printf("ff_m_getbuf: m_init return failed.\n");
		ff_pkt_free( (void*)m_bsd);
		return NULL;
	}
	
	return m_bsd;
}

struct mbuf* ff_m_gethdr(int how, unsigned short type)
{
	return ff_m_getbuf(how, type, M_PKTHDR);
}

struct mbuf* ff_m_getm(int how, unsigned short type)
{
	return ff_m_getbuf(how, type, 0);
}

struct mbuf* ff_m_getext(int how, unsigned short type)
{
	return ff_m_getbuf(how, type, M_EXT);
}

// prepend mbuf header to rte_mbuf, update mbuf header.
void* ff_embed_mhdr(void* pkt, unsigned short total, void *data,
		 unsigned short len, unsigned char rx_csum)
{
    struct mbuf *m = NULL;
    
    m = (struct mbuf*)ff_pkt_getbuf(pkt) ;
    if (  unlikely  (m_init(m, M_NOWAIT, MT_HEADER, 0) != 0) )
	{
		printf("ff_embed_mhdr: m_init return failed.\n");
		return NULL;
	}
g_mcnt ++;
    m_extadd(m, data, len, NULL, NULL, NULL, M_PKTHDR, 0);			// mbuf 不需要free ext部分。
    m->m_pkthdr.len = total;
    m->m_len = len;
    m->m_next = NULL;
    m->m_nextpkt = NULL;

    if (rx_csum) {
        m->m_pkthdr.csum_flags = CSUM_IP_CHECKED | CSUM_IP_VALID | CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
        m->m_pkthdr.csum_data = 0xffff;
    }
    return (void *)m;
}
extern long long g_mcnt ;
/**********************************************
 * set mbuf in received rte_mbuf's headroom.
 * | rte_mbuf | HEADROOM   256B  | buffer |
 * | rte_mbuf | BSD mbuf 136B    | data      | 
 * ********************************************/
void * ff_embed_mbuf(void *pkt, void *data, unsigned short len, void* prev)
{
    struct mbuf*	mb = NULL;
	struct mbuf*	m_pre = (struct mbuf*)prev;

    mb = (struct mbuf*) (uint64_t)ff_pkt_getbuf(pkt);
	m_extadd(mb, data, len, NULL, NULL, NULL, 0, 0);		// mbuf 不需要free ext部分，由rte_mbuf 层释放回收内存。
g_mcnt++;
    mb->m_next = NULL;
    mb->m_nextpkt = NULL;
    mb->m_len = len;

	m_pre->m_next = mb;
    return (void *)mb;
}

void ff_mext_attach(struct mbuf *m, void* buf, size_t len, int type)
{	
	m_extadd(m, buf, len, NULL, NULL, NULL, 0, type);
	m->m_len = len;
	ff_pkt_attach_extbuf(m, buf, len);
}

/*
 * Attach the cluster from *m to *n, set up m_ext in *n
 * and bump the refcount of the cluster.
 */
void ff_m_dupcl(struct mbuf *n, struct mbuf *m)
{
	mb_dupcl(n, m);
	ff_pkt_attach_extbuf(n, m->m_data, m->m_len);
}

/**********************************
 * user io_data pushed into tx buff. 
 * try to send a vector of io_data, align means how many bytes one op send.
 * one iov io_data can be divided into several mubfs.
 * caller should get the sent length which means how many bytes were sent.
 * ********************************/
struct mbuf* ff_uiotombuf(struct uio *uio, int how, int len, int align, int flags)
{
	struct mbuf *m, *prev, *top =NULL;
	int total = 0;
	ssize_t 	one_buf_len = 0;
	struct iovec* iov = NULL;

	if (align >= MHLEN)
		return (NULL);

	if (len > 0)
		total = min(uio->uio_resid, len);
	else
		total = uio->uio_resid;				// BSD support len <= 0, why???
	
	iov = uio->uio_iov;
	top = m = ff_m_getbuf(how, MT_DATA, flags);	
	if ( unlikely(!top) )
		return NULL;
	one_buf_len = total >= iov->iov_len  ? iov->iov_len : total;
	ff_mext_attach(m, iov->iov_base, one_buf_len, 0);
	uio->uio_offset += one_buf_len;
	uio->uio_resid -= one_buf_len;
	total -= one_buf_len;
	if ( total <= 0)
		return top;
	
	prev = m;
	while ( total > 0 )
	{
		uio->uio_iov++;
		iov = uio->uio_iov;
		if ( NULL == iov )
			break;
		m = ff_m_getbuf(how, MT_DATA, M_EXT);
		if ( unlikely(!m) )
			break;
		
		one_buf_len = total >= iov->iov_len ? iov->iov_len : total;
		ff_mext_attach(m, iov->iov_base, one_buf_len, 0);
		uio->uio_offset += one_buf_len;
		uio->uio_resid -= one_buf_len;
		
		prev->m_next = m;
		ff_pkt_setnext((void*)prev, (void*)m);
		prev = m;
		total -= one_buf_len;
	}
	return (top);
}

struct mbuf* ff_m_free(struct mbuf *m)
{
	struct mbuf *n = m->m_next;

	/*
	if ( likely(m->m_flags & M_EXT) )
		ff_mfree_ex(m);
	*/
	ff_pkt_free((void*)m);
g_mcnt --;
	return (n);
}

void ff_m_setnext(void* m, void* m_next)
{
	ff_pkt_setnext(m, m_next);
}

unsigned int ff_m_getsegs( struct mbuf* m )
{
	unsigned int n = 1;
	while ( m->m_next ) 
	{
		n++;
		m = m->m_next;
	}
	return n;
}

void* ff_m_sync_header(void* m)
{
	struct mbuf*  mhdr = m;
	KASSERT (((m)->m_flags & M_PKTHDR), "ff_m_sync_header invalid mbuf without PKT HEADER");

	return ff_pkt_sync_headr(mhdr, mhdr->m_data, mhdr->m_len, mhdr->m_pkthdr.len, ff_m_getsegs(mhdr) );
}

/************
 *  update the rte_mbuf chain following mbuf chain. 
 *  deprecated.
 * **********/
void* ff_m_chain(void* m)
{
	struct mbuf * m_cur = (struct mbuf *)m;
	struct mbuf * m_next = m_cur->m_next;
	unsigned int segs = 0;

	KASSERT( (m_cur->m_flags & M_PKTHDR), "ff_m_chain invalid mbuf without PKT HEADER");

	FF_SET_RTEMBUF(m_cur);
	segs = 1;
	while ( m_next )
	{
		ff_pkt_setnext(m_cur, m_next);
		FF_SET_RTEMBUF(m_next);
		segs += 1;
		m_cur = m_next;
		m_next = m_cur->m_next;
	}
g_mcnt -= segs;
	return ff_pkt_sethdr(m, segs);
}

/*
void ff_prepend_headr( struct mbuf* m, int len, int flags )
{
	KASSERT(m->m_flags & M_PKTHDR, "ff_prepend_headr input not header");
	M_PREPEND(m, len, flags);
	ff_sync_mhdr( m, m->m_data, m->m_len, m->m_pkthdr.len);
}
*/
