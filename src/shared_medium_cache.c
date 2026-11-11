#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/shared_medium_cache.h"
#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/arena_inlines.h"
#include "jemalloc/internal/arenas_management.h"
#include "jemalloc/internal/base.h"
#include "jemalloc/internal/large.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/prof.h"
#include "jemalloc/internal/safety_check.h"

bool   opt_experimental_shared_cache = false;
size_t opt_experimental_shared_cache_arena_group = 1;
size_t opt_experimental_shared_cache_group_max_bytes = 1536 * 1024;
size_t opt_experimental_shared_cache_idle_sec = 10;
bool   shared_medium_cache_active;

typedef struct {
	malloc_mutex_t mtx;
	edata_t      **slots;
	size_t         usize;
	unsigned       capacity;
	unsigned       count;
	unsigned       head;
	unsigned       low_water;
	uint64_t       hits;
	uint64_t       misses;
	uint64_t       puts;
	uint64_t       overflows;
	uint64_t       trims;
	uint64_t       trimmed_objects;
} shared_medium_cache_bin_t;

#define SHARED_MEDIUM_CACHE_DRAIN_BATCH 64
static shared_medium_cache_bin_t *bins;
static size_t                     ngroups, nbins, metadata_bytes;
static size_t                     first_usize;
static szind_t                    first_binind;
/* Also protects detached eviction batches from being lost across fork. */
static malloc_mutex_t maintenance_mtx;
static nstime_t       last_trim;

static shared_medium_cache_bin_t *
bin_get(size_t usize, unsigned arena_ind) {
	assert(usize >= first_usize && usize <= SHARED_MEDIUM_CACHE_MAX);
	assert(arena_ind < narenas_auto);
	size_t group = arena_ind / opt_experimental_shared_cache_arena_group;
	size_t sizeind = sz_large_size_classes_disabled()
	    ? (usize - first_usize) >> LG_PAGE
	    : sz_size2index(usize) - first_binind;
	assert(group < ngroups && sizeind * ngroups + group < nbins);
	return &bins[sizeind * ngroups + group];
}

bool
shared_medium_cache_boot(tsdn_t *tsdn) {
	if (!opt_experimental_shared_cache
	    || opt_experimental_shared_cache_group_max_bytes == 0
	    || PAGE > SHARED_MEDIUM_CACHE_MIN) {
		return false;
	}
	ngroups = (narenas_auto + opt_experimental_shared_cache_arena_group - 1)
	    / opt_experimental_shared_cache_arena_group;
	first_binind = (szind_t)max_zu(
	    SC_NBINS, sz_size2index(SHARED_MEDIUM_CACHE_MIN + 1));
	szind_t last_binind = sz_size2index(SHARED_MEDIUM_CACHE_MAX);
	if (first_binind > last_binind) {
		return false;
	}
	size_t nsizes = last_binind - first_binind + 1;
	first_usize = max_zu(SC_LARGE_MINCLASS, SHARED_MEDIUM_CACHE_MIN + PAGE);
	if (sz_large_size_classes_disabled()) {
		nsizes = ((SHARED_MEDIUM_CACHE_MAX - first_usize) >> LG_PAGE)
		    + 1;
	}
	if (malloc_mutex_init(&maintenance_mtx,
	        "shared_medium_cache_maintenance",
	        WITNESS_RANK_SHARED_MEDIUM_CACHE,
	        malloc_mutex_rank_exclusive)) {
		return true;
	}
	nbins = nsizes * ngroups;
	metadata_bytes = nbins * sizeof(*bins);
	bins = base_alloc(tsdn, b0get(), metadata_bytes, CACHELINE);
	if (bins == NULL) {
		return true;
	}
	memset(bins, 0, metadata_bytes);
	for (size_t i = 0; i < nsizes; i++) {
		size_t usize = sz_large_size_classes_disabled()
		    ? first_usize + (i << LG_PAGE)
		    : sz_index2size(first_binind + (szind_t)i);
		for (size_t group = 0; group < ngroups; group++) {
			shared_medium_cache_bin_t *bin =
			    &bins[i * ngroups + group];
			bin->usize = usize;
			bin->capacity =
			    (unsigned)(opt_experimental_shared_cache_group_max_bytes
			        / nsizes / (usize + sz_large_pad));
			if (malloc_mutex_init(&bin->mtx,
			        "shared_medium_cache_bin",
			        WITNESS_RANK_SHARED_MEDIUM_CACHE_BIN,
			        malloc_mutex_address_ordered)) {
				return true;
			}
			if (bin->capacity != 0) {
				size_t bytes = bin->capacity
				    * sizeof(*bin->slots);
				bin->slots = base_alloc(
				    tsdn, b0get(), bytes, CACHELINE);
				if (bin->slots == NULL) {
					return true;
				}
				metadata_bytes += bytes;
			}
		}
	}
	nstime_init_update(&last_trim);
	shared_medium_cache_active = true;
	return false;
}

void *
shared_medium_cache_alloc(
    tsd_t *tsd, arena_t *arena, size_t usize, bool zero, bool account_request) {
	assert(shared_medium_cache_active);
	if (arena_ind_get(arena) >= narenas_auto || !arena_is_auto(arena)) {
		return NULL;
	}
	usize = sz_s2u(usize);
	shared_medium_cache_bin_t *bin = bin_get(usize, arena_ind_get(arena));
	malloc_mutex_lock(tsd_tsdn(tsd), &bin->mtx);
	edata_t *edata = NULL;
	if (bin->count != 0) {
		bin->count--;
		edata = bin->slots[(bin->head + bin->count) % bin->capacity];
		bin->low_water = (unsigned)min_zu(bin->low_water, bin->count);
		bin->hits++;
	} else {
		bin->misses++;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &bin->mtx);
	if (edata == NULL) {
		return NULL;
	}
	assert(edata_usize_get(edata) == usize);
	if (config_stats && account_request) {
		arena_stats_large_nrequests_add(tsd_tsdn(tsd),
		    &arena_get_from_edata(edata)->stats, edata_szind_get(edata),
		    1);
	}
	void *ptr = edata_addr_get(edata);
	if (zero || (config_fill && opt_zero)) {
		memset(ptr, 0, usize);
	} else if (config_fill && opt_junk_alloc) {
		memset(ptr, JEMALLOC_ALLOC_JUNK, usize);
	}
	return ptr;
}

bool
shared_medium_cache_dalloc(tsd_t *tsd, edata_t *edata, void *ptr) {
	size_t usize = edata_usize_get(edata);
	if (edata_arena_ind_get(edata) >= narenas_auto
	    || !arena_is_auto(arena_get_from_edata(edata))
	    || edata_slab_get(edata) || edata_guarded_get(edata)
	    || (config_prof && opt_prof
	        && edata_prof_tctx_get(edata) != PROF_TCTX_SENTINEL)
	    || cache_bin_nonfast_aligned(ptr) || edata_addr_get(edata) != ptr
	    || edata_size_get(edata) != usize + sz_large_pad
	    || usize <= SHARED_MEDIUM_CACHE_MIN
	    || usize > SHARED_MEDIUM_CACHE_MAX) {
		return false;
	}
	if (config_fill && opt_junk_free) {
		memset(ptr, JEMALLOC_FREE_JUNK, usize);
	}
	shared_medium_cache_bin_t *bin = bin_get(
	    usize, edata_arena_ind_get(edata));
	malloc_mutex_lock(tsd_tsdn(tsd), &bin->mtx);
	if (config_opt_safety_checks) {
		for (unsigned i = 0; i < bin->count; i++) {
			if (bin->slots[(bin->head + i) % bin->capacity]
			    == edata) {
				malloc_mutex_unlock(tsd_tsdn(tsd), &bin->mtx);
				safety_check_fail(
				    "Double free in shared medium cache: %p\n",
				    ptr);
				return true;
			}
		}
	}
	bool retained = bin->count < bin->capacity;
	if (retained) {
		bin->slots[(bin->head + bin->count++) % bin->capacity] = edata;
		bin->puts++;
	} else {
		bin->overflows++;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &bin->mtx);
	return retained;
}

static void
drain_bin(tsd_t *tsd, shared_medium_cache_bin_t *bin, bool force) {
	edata_t *victims[SHARED_MEDIUM_CACHE_DRAIN_BATCH];
	malloc_mutex_lock(tsd_tsdn(tsd), &bin->mtx);
	unsigned target = force
	    ? bin->count
	    : (unsigned)min_zu(bin->low_water, SHARED_MEDIUM_CACHE_DRAIN_BATCH);
	if (target != 0) {
		bin->trims++;
	}
	while (target != 0 && bin->count != 0) {
		unsigned n = (unsigned)min_zu(target,
		    min_zu(bin->count, SHARED_MEDIUM_CACHE_DRAIN_BATCH));
		for (unsigned i = 0; i < n; i++) {
			victims[i] =
			    bin->slots[(bin->head + i) % bin->capacity];
		}
		bin->head = (bin->head + n) % bin->capacity;
		bin->count -= n;
		bin->trimmed_objects += n;
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->mtx);
		for (unsigned i = 0; i < n; i++) {
			/* The original arena and PAI remain attached to the extent. */
			large_dalloc(tsd_tsdn(tsd), victims[i]);
		}
		target -= n;
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->mtx);
	}
	bin->low_water = bin->count;
	malloc_mutex_unlock(tsd_tsdn(tsd), &bin->mtx);
}

static void
drain(tsd_t *tsd, bool force) {
	for (size_t i = 0; i < nbins; i++) {
		drain_bin(tsd, &bins[i], force);
	}
	nstime_init_update(&last_trim);
}

void
shared_medium_cache_flush(tsd_t *tsd) {
	if (shared_medium_cache_active) {
		malloc_mutex_lock(tsd_tsdn(tsd), &maintenance_mtx);
		drain(tsd, true);
		malloc_mutex_unlock(tsd_tsdn(tsd), &maintenance_mtx);
	}
}

void
shared_medium_cache_trim(tsd_t *tsd) {
	if (shared_medium_cache_active) {
		malloc_mutex_lock(tsd_tsdn(tsd), &maintenance_mtx);
		drain(tsd, false);
		malloc_mutex_unlock(tsd_tsdn(tsd), &maintenance_mtx);
	}
}

void
shared_medium_cache_background_work(tsd_t *tsd) {
	if (!shared_medium_cache_active
	    || malloc_mutex_trylock(tsd_tsdn(tsd), &maintenance_mtx)) {
		return;
	}
	nstime_t now;
	nstime_init_update(&now);
	if (nstime_ns(&now) - nstime_ns(&last_trim)
	    >= opt_experimental_shared_cache_idle_sec * KQU(1000000000)) {
		drain(tsd, false);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &maintenance_mtx);
}

void
shared_medium_cache_stats_get(tsd_t *tsd, shared_medium_cache_stats_t *stats) {
	memset(stats, 0, sizeof(*stats));
	if (!shared_medium_cache_active) {
		return;
	}
	stats->groups = ngroups;
	stats->metadata_bytes = metadata_bytes;
	for (size_t i = 0; i < nbins; i++) {
		shared_medium_cache_bin_t *bin = &bins[i];
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->mtx);
		stats->cached_bytes += bin->count * bin->usize;
		stats->cached_extent_bytes += bin->count
		    * (bin->usize + sz_large_pad);
		stats->capacity_bytes += bin->capacity
		    * (bin->usize + sz_large_pad);
		stats->hits += bin->hits;
		stats->misses += bin->misses;
		stats->puts += bin->puts;
		stats->overflows += bin->overflows;
		stats->trims += bin->trims;
		stats->trimmed_objects += bin->trimmed_objects;
		stats->lock_ops += bin->mtx.prof_data.n_lock_ops;
		stats->lock_wait_ns += nstime_ns(
		    &bin->mtx.prof_data.tot_wait_time);
		stats->lock_waits += bin->mtx.prof_data.n_wait_times;
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->mtx);
	}
}

void
shared_medium_cache_prefork(tsd_t *tsd) {
	if (shared_medium_cache_active) {
		malloc_mutex_prefork(tsd_tsdn(tsd), &maintenance_mtx);
		for (size_t i = 0; i < nbins; i++) {
			malloc_mutex_prefork(tsd_tsdn(tsd), &bins[i].mtx);
		}
	}
}

void
shared_medium_cache_postfork_parent(tsd_t *tsd) {
	if (shared_medium_cache_active) {
		for (size_t i = nbins; i-- > 0;) {
			malloc_mutex_postfork_parent(
			    tsd_tsdn(tsd), &bins[i].mtx);
		}
		malloc_mutex_postfork_parent(tsd_tsdn(tsd), &maintenance_mtx);
	}
}

void
shared_medium_cache_postfork_child(tsd_t *tsd) {
	if (shared_medium_cache_active) {
		for (size_t i = nbins; i-- > 0;) {
			malloc_mutex_postfork_child(
			    tsd_tsdn(tsd), &bins[i].mtx);
		}
		nstime_init_update(&last_trim);
		malloc_mutex_postfork_child(tsd_tsdn(tsd), &maintenance_mtx);
	}
}
