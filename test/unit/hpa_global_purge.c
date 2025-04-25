#include "test/jemalloc_test.h"

#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/nstime.h"

#define SHARD_IND 111

#define ALLOC_MAX (HUGEPAGE)

typedef struct test_data_s test_data_t;
struct test_data_s {
	/*
	 * Must be the first member -- we convert back and forth between the
	 * test_data_t and the hpa_shard_t;
	 */
	hpa_shard_t shard;
	hpa_central_t central;
	base_t *base;
	edata_cache_t shard_edata_cache;

	emap_t emap;
};

static hpa_shard_opts_t test_hpa_shard_opts_default = {
	/* slab_max_alloc */
	ALLOC_MAX,
	/* hugification_threshold */
	HUGEPAGE,
	/* dirty_mult */
	FXP_INIT_PERCENT(25),
	/* deferral_allowed */
	false,
	/* hugify_delay_ms */
	10 * 1000,
	/* hugify_sync */
	false,
	/* min_purge_interval_ms */
	5 * 1000,
	/* experimental_max_purge_nhp */
	-1,
	/* peak_demand_window_ms */
	0
};

static hpa_shard_t *
create_test_data(const hpa_hooks_t *hooks, hpa_shard_opts_t *opts) {
	bool err;
	base_t *base = base_new(TSDN_NULL, /* ind */ SHARD_IND,
	    &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);
	assert_ptr_not_null(base, "");

	test_data_t *test_data = malloc(sizeof(test_data_t));
	assert_ptr_not_null(test_data, "");

	test_data->base = base;

	err = edata_cache_init(&test_data->shard_edata_cache, base);
	assert_false(err, "");

	err = emap_init(&test_data->emap, test_data->base, /* zeroed */ false);
	assert_false(err, "");

	err = hpa_central_init(&test_data->central, test_data->base, hooks);
	assert_false(err, "");

	err = hpa_shard_init(&test_data->shard, &test_data->central,
	    &test_data->emap, test_data->base, &test_data->shard_edata_cache,
	    SHARD_IND, opts);
	assert_false(err, "");

	return (hpa_shard_t *)test_data;
}

static void
destroy_test_data(hpa_shard_t *shard) {
	test_data_t *test_data = (test_data_t *)shard;
	base_delete(TSDN_NULL, test_data->base);
	free(test_data);
}

static uintptr_t defer_bump_ptr = HUGEPAGE * 123;
static void *
defer_test_map(size_t size) {
	void *result = (void *)defer_bump_ptr;
	defer_bump_ptr += size;
	return result;
}

static void
defer_test_unmap(void *ptr, size_t size) {
	(void)ptr;
	(void)size;
}

static void
defer_test_purge(void *ptr, size_t size) {
	(void)ptr;
	(void)size;
}

static bool
defer_vectorized_purge(void *vec, size_t vlen, size_t nbytes) {
	(void)vec;
	(void)nbytes;
	return false;
}

static bool
defer_test_hugify(void *ptr, size_t size, bool sync) {
	return false;
}

static void
defer_test_dehugify(void *ptr, size_t size) {
}

static nstime_t defer_curtime;
static void
defer_test_curtime(nstime_t *r_time, bool first_reading) {
	*r_time = defer_curtime;
}

static uint64_t
defer_test_ms_since(nstime_t *past_time) {
	return (nstime_ns(&defer_curtime) - nstime_ns(past_time)) / 1000 / 1000;
}

TEST_BEGIN(test_read_analytics) {
	test_skip_if(!hpa_supported() || !config_stats || HUGEPAGE_PAGES <4);

	opt_hpa_purge_policy = hpa_purge_policy_global_ratio;

        hpa_hooks_t hooks;
	hooks.map = &defer_test_map;
	hooks.unmap = &defer_test_unmap;
	hooks.purge = &defer_test_purge;
	hooks.hugify = &defer_test_hugify;
	hooks.dehugify = &defer_test_dehugify;
	hooks.curtime = &defer_test_curtime;
	hooks.ms_since = &defer_test_ms_since;
	hooks.vectorized_purge = &defer_vectorized_purge;

	hpa_shard_opts_t opts = test_hpa_shard_opts_default;
	opts.deferral_allowed = true;
	opts.min_purge_interval_ms = 0;
	opts.peak_demand_window_ms = 10 * 1000;

	hpa_shard_t *shard = create_test_data(&hooks, &opts);

	bool deferred_work_generated = false;

	nstime_init(&defer_curtime, 0);
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());

	enum {NALLOCS = 8 * HUGEPAGE_PAGES};
	edata_t *edatas[NALLOCS];
	for (int i = 0; i < NALLOCS; i++) {
		edatas[i] = pai_alloc(tsdn, &shard->pai, PAGE, PAGE, false,
		    false, false, &deferred_work_generated);
		expect_ptr_not_null(edatas[i], "Unexpected null edata");
	}
	/* Deallocate almost 3 pages out of 8, and to force batching
	 * leave the 2nd and 4th PAGE in the first 3 hugepages.
	 */
	for (int i = 0; i < 3 * (int)HUGEPAGE_PAGES; i++) {
		int j = i % HUGEPAGE_PAGES;
		if (j != 1 && j != 3) {
			pai_dalloc(tsdn, &shard->pai, edatas[i],
			    &deferred_work_generated);
		}
	}
	nstime_init2(&defer_curtime, 12, 0);
	
	hpa_purge_analytics_t analytics;
	expect_true(hpa_purge_analytics_read(tsdn, shard, &analytics), "");
	expect_true(analytics.shard == shard, "");
	size_t total_full = 0;
	size_t total_empty = 0;
	size_t total_nonfull = 0;
	
	total_full += analytics.stats.full_slabs[0].npageslabs;
	total_full += analytics.stats.full_slabs[1].npageslabs;

	total_empty += analytics.stats.empty_slabs[0].npageslabs;
	total_empty += analytics.stats.empty_slabs[1].npageslabs;

	for (size_t i = 0; i < PSSET_NPSIZES; ++i) {
		total_nonfull += analytics.stats.nonfull_slabs[i][0].npageslabs;
		total_nonfull += analytics.stats.nonfull_slabs[i][1].npageslabs;
	}
	expect_zu_eq(0, total_empty, "");
	expect_zu_eq(5, total_full, "");
	expect_zu_eq(3, total_nonfull, "");

	destroy_test_data(shard);
}
TEST_END

TEST_BEGIN(test_analyze) {
	hpa_purge_analytics_t analytics[2];
	opt_hpa_purge_policy = hpa_purge_policy_global_ratio;

	analytics[0].dirty_mult = FXP_INIT_PERCENT(25);
	analytics[0].peak_max  = 8;
	analytics[0].stats.merged.nactive = 2;
	analytics[0].stats.merged.ndirty = 12;

	analytics[1].dirty_mult = FXP_INIT_PERCENT(25);
	analytics[1].peak_max  = 12;
	analytics[1].stats.merged.nactive = 10;
	analytics[1].stats.merged.ndirty = 32;

	size_t target_dirty = 0;
	expect_true(hpa_purge_analyze(&analytics[0], 2, &target_dirty), "");
	size_t peak_with_slack = (12 + 8) + fxp_mul_frac(12 + 8, FXP_INIT_PERCENT(25));
	size_t dirty = 32 + 12;
	size_t active = 2 + 10;
	size_t dirty_max = peak_with_slack - (active);
	size_t expected_target = dirty - dirty_max;
	expect_zu_eq(expected_target, target_dirty, "");
} TEST_END

static void
init_analytics(hpa_purge_analytics_t *panalytics, size_t nempty_huge,
	       size_t nempty_nonhuge, size_t above_peakmax,
	       size_t a, size_t b,
	       size_t expected_order) {
	/* We will never use this to fetch arena, so we use it for expected order */
	panalytics->arena_ind = expected_order;

	panalytics->stats.empty_slabs[0].npageslabs = nempty_nonhuge;
	panalytics->stats.empty_slabs[1].npageslabs = nempty_huge;
	panalytics->stats.nonfull_slabs[0][0].ndirty = 0;
}


typedef struct hpa_purge_analysis_result_s hpa_purge_analysis_result_t;
struct hpa_purge_analysis_result_s {
	size_t n
};

void analyze(hpa_purge_analytics_t *panalytics, size_t n) {
}


TEST_BEGIN(test_global_ratio_policy) {
	/* Create N analytics data pieces
	   empty_huge, empty_nh, nonfull_nonhuge_dirty, nf_huge_dirty, above_peakmax, Label, expected_order
	   10, 20, 50, 30, 256, A, 3
	   10, 20, 50, 30, 257, C, 2
	   10, 20, 50, 25, 300, D, 4
	   10, 20, 50, 35, 100, E, 5
	   10, 20, 40, 100, 500, F, 6
	   10, 15, 100, 0, 0, G, 7
	   10, 25, 0, 1000, 0, I, 0
	   11, 0, 0, 0, 100, J, 1	   
	 */
	hpa_purge_analytics_t analytics[9];
	init_analytics(&analytics[0], 10, 20, 50, 30, 256, 3);
	init_analytics(&analytics[1], 10, 20, 50, 30, 257, 2);
	init_analytics(&analytics[2], 10, 20, 50, 25, 300, 4);
	init_analytics(&analytics[3], 10, 20, 50, 35, 100, 5);
	init_analytics(&analytics[4], 10, 20, 40, 100, 500, 6);
	init_analytics(&analytics[5], 10, 15, 100, 0, 0, 7);
	init_analytics(&analytics[6], 10, 25, 0, 1000, 0, 0);
	init_analytics(&analytics[7], 11, 0, 0, 0, 100, 1);
	

	
	/* Calculate total target and create heaps based on:
	   number of empty huge,
	   number of empty non-huge,
	   number of dirty on non_huge slabs,
	   number of dirty overall - active max
	*/
	/* While target not achived, pop one from ph
	   hit purge of all you can on that shard.
	   Repeat until not possible to purge anymore
	   So at most two passes through heap */
	   
	   
    	opt_hpa_purge_policy = hpa_purge_policy_global_ratio;

	analytics[0].dirty_mult = FXP_INIT_PERCENT(25);
	analytics[0].peak_max  = 8;
	analytics[0].stats.merged.nactive = 2;
	analytics[0].stats.merged.ndirty = 12;

	analytics[1].dirty_mult = FXP_INIT_PERCENT(25);
	analytics[1].peak_max  = 12;
	analytics[1].stats.merged.nactive = 10;
	analytics[1].stats.merged.ndirty = 32;

	size_t target_dirty = 0;
	expect_true(hpa_purge_analyze(&analytics[0], 2, &target_dirty), "");
	size_t peak_with_slack = (12 + 8) + fxp_mul_frac(12 + 8, FXP_INIT_PERCENT(25));
	size_t dirty = 32 + 12;
	size_t active = 2 + 10;
	size_t dirty_max = peak_with_slack - (active);
	size_t expected_target = dirty - dirty_max;
	expect_zu_eq(expected_target, target_dirty, "");
} TEST_END



int
main(void) {
	return test_no_reentrancy(
		test_read_analytics,
		test_analyze,
		test_global_ratio_policy);
}
