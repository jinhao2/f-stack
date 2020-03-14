/**********************************
 * this is  ret_mbuf's functions,  used by ff_mbuf_api functions.
 * 
 * ********************************/
#ifndef	_FF_RTEMBUF_API_
#define _FF_RTEMBUF_API_
#ifdef __cplusplus
extern "C" {
#endif

enum{
    MBUF_EXT_T = 0x00000001,
    MBUF_HDR_T = 0x00000002,
};
#define Hdr_Mbuf_Num	2048
#define Hdr_Mbuf_SZ		1024
#define EXT_Mbuf_Num	4096

#define DPDK_MBUF_FLAG	0x00000100

//#define	M_UNUSED_8	0x00000100 /* --available-- */

/**
 * Check if a branch is likely to be taken.
 *
 * This compiler builtin allows the developer to indicate if a branch is
 * likely to be taken. Example:
 *
 *   if (likely(x > 1))
 *      do_stuff();
 *
 */
#ifndef likely
#define likely(x)	__builtin_expect(!!(x), 1)
#endif /* likely */

/**
 * Check if a branch is unlikely to be taken.
 *
 * This compiler builtin allows the developer to indicate if a branch is
 * unlikely to be taken. Example:
 *
 *   if (unlikely(x < 1))
 *      do_stuff();
 *
 */
#ifndef unlikely
#define unlikely(x)	__builtin_expect(!!(x), 0)
#endif /* unlikely */

int ff_mbuf_pool_init(const char* app_name);
void* ff_pkt_new(int flags);
void* ff_pkt_getbuf( void* pkt );
void ff_pkt_attach_extbuf( void* m , void* buf, int len);

void ff_pkt_free(void* m);
void ff_pkt_setnext(void* prev, void* m);
void ff_pkt_setlen(void* m, void* mdata, int mlen, int pktlen);
void* ff_pkt_sync_headr( void* m, void* m_data, uint16_t m_len, uint32_t pkt_len, uint32_t segs);
void* ff_pkt_sethdr( void* m, unsigned int segs );
#ifdef __cplusplus
}
#endif

#endif
