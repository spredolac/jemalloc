#ifndef JEMALLOC_INTERNAL_HPA_OPTS_H
#define JEMALLOC_INTERNAL_HPA_OPTS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/fxp.h"

/*
 * This file is morally part of hpa.h, but is split out for header-ordering
 * reasons.
 */

typedef struct hpa_shard_opts_s hpa_shard_opts_t;
struct hpa_shard_opts_s {
	/*
	 * The largest size we'll allocate out of the shard.  For those
	 * allocations refused, the caller (in practice, the PA module) will
	 * fall back to the more general (for now) PAC, which can always handle
	 * any allocation request.
	 */
	size_t slab_max_alloc;

	/*
	 * When the number of active bytes in a hugepage is >=
	 * hugification_threshold, we force hugify it.
	 */
	size_t hugification_threshold;

	/*
	 * The HPA purges whenever the number of pages exceeds dirty_mult *
	 * active_pages.  This may be set to (fxp_t)-1 to disable purging.
	 */
	fxp_t dirty_mult;

	/*
	 * Whether or not the PAI methods are allowed to defer work to a
	 * subsequent hpa_shard_do_deferred_work() call.  Practically, this
	 * corresponds to background threads being enabled.  We track this
	 * ourselves for encapsulation purposes.
	 */
	bool deferral_allowed;

	/*
	 * How long a hugepage has to be a hugification candidate before it will
	 * actually get hugified.
	 */
	uint64_t hugify_delay_ms;

	/*
	 * Hugify pages synchronously (using MADV_COLLAPSE).
	 */
	bool hugify_sync;

	/*
	 * Minimum amount of time between purges.
	 */
	uint64_t min_purge_interval_ms;

	/*
	 * Maximum number of hugepages to purge on each purging attempt.
	 */
	ssize_t experimental_max_purge_nhp;

	/*
	 * When the number of inactive bytes in a hugepage is >=
	 * purge_threshold, the page is purgable.  Setting this to 1 will allow
	 * every page to be purged, while setting it to HUGEPAGE would only
	 * purge completely empty pages.  Depending on your kernel settings
	 * purging from non-empty hugepage may result in loss of performance.
	 */
	size_t purge_threshold;

	/*
	 * How long does HP page need to be eligible for purging before it gets
	 * purged.  Setting this to larger number would give better chance of
	 * reusing that memory.  Setting it to 0 means that page is eligible
	 * for purging as soon as it meets the purge_threshold.
	 */
	uint64_t purge_delay_ticks;

	/*
	 * If page should start as huge (instead of waiting to for hugification
	 * threshold to be reached).  This allows us to utilize HP immediately
	 * and have similar behavior whether the thp setting is 'always' or
	 * 'madvise'.  When using this option you probably want to purge less
	 * aggressively: either no purge at all (dirty_mult=-1), or purge only
	 * empty pages (purge_threshold=HUGEPAGE) with some delay that allows
	 * their reuse (for example the period between memory peaks).
	 */
        bool start_as_huge;
};

/* clang-format off */
#define HPA_SHARD_OPTS_DEFAULT {					\
	/* slab_max_alloc */						\
	64 * 1024,							\
	/* hugification_threshold */					\
	HUGEPAGE * 95 / 100,						\
	/* dirty_mult */						\
	FXP_INIT_PERCENT(25),						\
	/*								\
	 * deferral_allowed						\
	 * 								\
	 * Really, this is always set by the arena during creation	\
	 * or by an hpa_shard_set_deferral_allowed call, so the value	\
	 * we put here doesn't matter.					\
	 */								\
	false,								\
	/* hugify_delay_ms */						\
	10 * 1000,							\
	/* hugify_sync */						\
	false,								\
	/* min_purge_interval_ms */					\
	5 * 1000,							\
	/* experimental_max_purge_nhp */				\
	-1,      							\
	/* size_t purge_threshold */					\
	1,								\
	/* purge_delay_ticks */             				\
	0,  								\
	/* start_as_huge */                    				\
	false								\
}
/* clang-format on */

#endif /* JEMALLOC_INTERNAL_HPA_OPTS_H */
