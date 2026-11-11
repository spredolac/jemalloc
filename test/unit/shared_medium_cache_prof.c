#include "test/jemalloc_test.h"

#include "jemalloc/internal/shared_medium_cache.h"

const char *malloc_conf =
    "prof:true,prof_active:false,lg_prof_sample:0,narenas:2,"
    "percpu_arena:disabled,tcache_max:32768,experimental_shared_cache:true";

TEST_BEGIN(test_samples_bypass_cache) {
	test_skip_if(!config_prof || !shared_medium_cache_active);
	shared_medium_cache_flush(tsd_fetch());
	void *ptr = mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
	assert_ptr_not_null(ptr, "Allocation failed");
	dallocx(ptr, 0);
	shared_medium_cache_stats_t before;
	shared_medium_cache_stats_get(tsd_fetch(), &before);
	bool active = true;
	expect_d_eq(mallctl("prof.active", NULL, NULL, &active, sizeof(active)),
	    0, "Cannot enable sampling");
	void *sampled = mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
	assert_ptr_not_null(sampled, "Sampled allocation failed");
	expect_ptr_ne(sampled, ptr, "Sample consumed cached object");
	edata_t *edata = emap_edata_lookup(
	    tsdn_fetch(), &arena_emap_global, sampled);
	expect_ptr_ne(
	    edata_prof_tctx_get(edata), PROF_TCTX_SENTINEL, "Expected sample");
	dallocx(sampled, 0);
	active = false;
	expect_d_eq(mallctl("prof.active", NULL, NULL, &active, sizeof(active)),
	    0, "Cannot disable sampling");
	shared_medium_cache_stats_t after;
	shared_medium_cache_stats_get(tsd_fetch(), &after);
	expect_u64_eq(after.hits, before.hits, "Sample hit cache");
	expect_u64_eq(after.puts, before.puts, "Sample retained in cache");
	void *reused = mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
	expect_ptr_eq(reused, ptr, "Unsampled object not reused");
	dallocx(reused, 0);
	shared_medium_cache_flush(tsd_fetch());
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_samples_bypass_cache);
}
