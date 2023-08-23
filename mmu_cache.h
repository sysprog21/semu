#pragma once
#include <string.h>
/* This currently implements a simple, fixed-size LRU cache for MMU
   entries. Assumes that address 0 is never cached and it is used to
   flag empty entries. If a returned translation is zero, the calling
   logic has to assume that this means 'no entry available'. */

#define MMU_CACHE_ENTRIES 3
#define MMU_CACHE_IDX_TYPE int
#define MMU_CACHE_STATS 0

struct _mmu_cache_ctx {
    MMU_CACHE_IDX_TYPE write_idx;
    uint32_t from[MMU_CACHE_ENTRIES];
    uint32_t to[MMU_CACHE_ENTRIES];
#if MMU_CACHE_STATS
    uint64_t hits, misses;
#endif
};

static inline void mmu_cache_reset_ctx(struct _mmu_cache_ctx* cc) {
#if MMU_CACHE_ENTRIES > 0
    cc->write_idx=0;
    for (size_t i=0; i != MMU_CACHE_ENTRIES; i++) {
	cc->from[i]=cc->to[i]=0;
    }
#endif
}

static inline uint32_t mmu_cache_lookup(struct _mmu_cache_ctx* cc,
				 uint32_t high_part) {
#if MMU_CACHE_ENTRIES > 0
    MMU_CACHE_IDX_TYPE i = cc->write_idx;
    do {
	if (cc->from[i] == high_part) {
#if MMU_CACHE_STATS
	    cc->hits++;
#endif
	    return cc->to[i];
	}
	i--;
	if (i<0) i = MMU_CACHE_ENTRIES-1;
    } while(i != cc->write_idx);
#endif
#if MMU_CACHE_STATS
    cc->misses++;
#endif
    return 0; // no entry available
}

static inline void mmu_cache_insert(struct _mmu_cache_ctx* cc,
				    uint32_t ifrom,
				    uint32_t ito) {
#if MMU_CACHE_ENTRIES > 0
    MMU_CACHE_IDX_TYPE idx = cc->write_idx+1;
    if (idx == MMU_CACHE_ENTRIES)
	idx = 0;
    cc->write_idx = idx;
    cc->from[idx]=ifrom;
    cc->to[idx]=ito;
#endif
}
