#ifndef JEMALLOC_INTERNAL_SHARED_MEDIUM_CACHE_H
#define JEMALLOC_INTERNAL_SHARED_MEDIUM_CACHE_H

#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/tcache.h"
#include "jemalloc/internal/tsd.h"

#define SHARED_MEDIUM_CACHE_MIN (16 * 1024)
#define SHARED_MEDIUM_CACHE_MAX (64 * 1024)

extern bool   opt_experimental_shared_cache;
extern size_t opt_experimental_shared_cache_arena_group;
extern size_t opt_experimental_shared_cache_group_max_bytes;
extern size_t opt_experimental_shared_cache_idle_sec;
extern bool   shared_medium_cache_active;

typedef struct {
	size_t   groups;
	size_t   cached_bytes;
	size_t   cached_extent_bytes;
	size_t   capacity_bytes;
	size_t   metadata_bytes;
	uint64_t hits;
	uint64_t misses;
	uint64_t puts;
	uint64_t overflows;
	uint64_t trims;
	uint64_t trimmed_objects;
	uint64_t lock_ops;
	uint64_t lock_wait_ns;
	uint64_t lock_waits;
} shared_medium_cache_stats_t;

bool shared_medium_cache_boot(tsdn_t *tsdn);
/* Tcache refills account for their own requests; direct hits need accounting. */
void *shared_medium_cache_alloc(
    tsd_t *tsd, arena_t *arena, size_t usize, bool zero, bool account_request);
bool shared_medium_cache_dalloc(tsd_t *tsd, edata_t *edata, void *ptr);
void shared_medium_cache_flush(tsd_t *tsd);
void shared_medium_cache_trim(tsd_t *tsd);
void shared_medium_cache_background_work(tsd_t *tsd);
void shared_medium_cache_stats_get(
    tsd_t *tsd, shared_medium_cache_stats_t *stats);
void shared_medium_cache_prefork(tsd_t *tsd);
void shared_medium_cache_postfork_parent(tsd_t *tsd);
void shared_medium_cache_postfork_child(tsd_t *tsd);

JEMALLOC_ALWAYS_INLINE bool
shared_medium_cache_eligible(tsdn_t *tsdn, tcache_t *tcache, size_t usize) {
	return shared_medium_cache_active && usize > SHARED_MEDIUM_CACHE_MIN
	    && usize <= SHARED_MEDIUM_CACHE_MAX && !tsdn_null(tsdn)
	    && tcache != NULL && tcache == tsd_tcachep_get(tsdn_tsd(tsdn))
	    && tsd_state_get(tsdn_tsd(tsdn)) <= tsd_state_nominal_max
	    && tsd_tcache_enabled_get(tsdn_tsd(tsdn))
	    && tsd_reentrancy_level_get(tsdn_tsd(tsdn)) == 0;
}

#endif /* JEMALLOC_INTERNAL_SHARED_MEDIUM_CACHE_H */
