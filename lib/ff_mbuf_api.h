
#ifndef	_FF_MBUF_API_
#define _FF_MBUF_API_

#ifdef __cplusplus
extern "C" {
#endif

struct mbuf* ff_m_gethdr(int how, unsigned short type);
struct mbuf* ff_m_getm(int how, unsigned short type);
struct mbuf* ff_m_getext(int how, unsigned short type);
void* ff_embed_mhdr(void* pkt, unsigned short total, void *data, unsigned short len, unsigned char rx_csum);
void * ff_embed_mbuf(void *pkt, void *data, unsigned short len, void* prev);
void ff_mext_attach(struct mbuf *m, void* addr, size_t len, int type);
void ff_m_dupcl(struct mbuf *n, struct mbuf *m);
struct mbuf* ff_uiotombuf(struct uio *uio, int how, int len, int align, int flags);
void ff_mfree_ex(struct mbuf *m);
struct mbuf* ff_m_free(struct mbuf *m);
extern void ff_mbuf_ext_free(struct mbuf *m, void *arg1, void *arg2);

void ff_m_setnext(void* m, void* m_next);
void* ff_m_sync_header(void* m);
void* ff_m_chain(void* m);

#ifdef __cplusplus
}
#endif

#endif


