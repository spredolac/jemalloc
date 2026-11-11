#include "test/jemalloc_test.h"

#include "jemalloc/internal/shared_medium_cache.h"

#ifndef _WIN32
#	include <sys/wait.h>
#endif

const char *malloc_conf =
    "narenas:2,percpu_arena:disabled,tcache_max:32768,"
    "experimental_shared_cache:true,experimental_shared_cache_arena_group:2,"
    "experimental_shared_cache_idle_sec:1";

static shared_medium_cache_stats_t
read_stats(void) {
	shared_medium_cache_stats_t stats;
	size_t                      len = sizeof(stats);
	expect_d_eq(
	    mallctl("experimental.shared_cache.stats", &stats, &len, NULL, 0),
	    0, "Cannot read shared cache stats");
	return stats;
}

static void
flush_tcache(void) {
	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0), 0,
	    "Cannot flush tcache");
}

static void
flush_shared(void) {
	expect_d_eq(
	    mallctl("experimental.shared_cache.flush", NULL, NULL, NULL, 0), 0,
	    "Cannot flush shared cache");
}

static void
bind_arena(unsigned ind) {
	expect_d_eq(mallctl("thread.arena", NULL, NULL, &ind, sizeof(ind)), 0,
	    "Cannot bind arena");
}

TEST_BEGIN(test_reuse_and_zero) {
	test_skip_if(!shared_medium_cache_active);
	flush_tcache();
	flush_shared();
	for (size_t size = SHARED_MEDIUM_CACHE_MIN + PAGE;
	    size <= SHARED_MEDIUM_CACHE_MAX; size += PAGE) {
		if (sz_can_use_slab(size) || sz_s2u(size) != size) {
			continue;
		}
		void *ptr = mallocx(size, 0);
		assert_ptr_not_null(ptr, "Allocation failed");
		memset(ptr, 0x5a, size);
		edata_t *owner = emap_edata_lookup(
		    tsdn_fetch(), &arena_emap_global, ptr);
		if (opt_hpa
		    && size + sz_large_pad <= opt_hpa_opts.slab_max_alloc) {
			expect_u_eq(edata_pai_get(owner), EXTENT_PAI_HPA,
			    "HPA backing not exercised");
		}
		dallocx(ptr, 0);
		flush_tcache();
		expect_zu_eq(
		    read_stats().cached_bytes, size, "Object not retained");
		void *reused = mallocx(size, MALLOCX_ZERO);
		expect_ptr_eq(
		    reused, ptr, "Shared cache did not reuse the object");
		expect_ptr_eq(
		    emap_edata_lookup(tsdn_fetch(), &arena_emap_global, reused),
		    owner, "Reuse changed extent ownership");
		for (size_t i = 0; i < size; i++) {
			assert_u_eq(((unsigned char *)reused)[i], 0,
			    "Zero allocation not zeroed");
		}
		dallocx(reused, 0);
		flush_tcache();
		flush_shared();
		expect_zu_eq(
		    read_stats().cached_bytes, 0, "Flush left cached objects");
	}
}
TEST_END

static bool double_free_detected;

static void
record_double_free(const char *message) {
	(void)message;
	double_free_detected = true;
}

TEST_BEGIN(test_double_free) {
	test_skip_if(!shared_medium_cache_active || !config_opt_safety_checks);
	flush_tcache();
	flush_shared();
	void *ptr = mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
	assert_ptr_not_null(ptr, "Allocation failed");
	dallocx(ptr, 0);
	double_free_detected = false;
	test_hooks_safety_check_abort = record_double_free;
	dallocx(ptr, 0);
	test_hooks_safety_check_abort = NULL;
	expect_true(
	    double_free_detected, "Did not detect cached-object double free");
	expect_zu_eq(read_stats().cached_bytes, SHARED_MEDIUM_CACHE_MAX,
	    "Double free changed cache inventory");
	flush_shared();
}
TEST_END

static uint64_t
large_requests(void) {
	uint64_t epoch = 1, requests;
	expect_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Cannot refresh stats");
	char key[80];
	malloc_snprintf(key, sizeof(key),
	    "stats.arenas.0.lextents.%u.nrequests",
	    (unsigned)(sz_size2index(SHARED_MEDIUM_CACHE_MAX) - SC_NBINS));
	size_t len = sizeof(requests);
	expect_d_eq(
	    mallctl(key, &requests, &len, NULL, 0), 0, "Cannot read requests");
	return requests;
}

TEST_BEGIN(test_request_accounting) {
	test_skip_if(!shared_medium_cache_active || !config_stats);
	flush_tcache();
	flush_shared();
	bind_arena(0);
	void *ptr = mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
	assert_ptr_not_null(ptr, "Allocation failed");
	dallocx(ptr, 0);
	uint64_t before = large_requests();
	void    *reused = mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
	expect_ptr_eq(reused, ptr, "Expected a cache hit");
	expect_u64_eq(
	    large_requests(), before + 1, "Direct hit not counted once");
	dallocx(reused, 0);
	flush_shared();
}
TEST_END

TEST_BEGIN(test_bounds_and_bypasses) {
	test_skip_if(!shared_medium_cache_active);
	flush_tcache();
	flush_shared();
	void *ptrs[128];
	for (size_t i = 0; i < ARRAY_SIZE(ptrs); i++) {
		ptrs[i] = mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
		assert_ptr_not_null(ptrs[i], "Allocation failed");
	}
	uint64_t overflows = read_stats().overflows;
	for (size_t i = 0; i < ARRAY_SIZE(ptrs); i++) {
		dallocx(ptrs[i], 0);
	}
	shared_medium_cache_stats_t stats = read_stats();
	expect_u64_gt(
	    stats.overflows, overflows, "Expected bounded-cache overflow");
	expect_zu_le(stats.cached_extent_bytes, stats.capacity_bytes,
	    "Capacity exceeded");
	expect_zu_le(stats.capacity_bytes,
	    stats.groups * opt_experimental_shared_cache_group_max_bytes,
	    "Budget exceeded");
	flush_shared();
	const size_t outside[] = {
	    SHARED_MEDIUM_CACHE_MIN, SHARED_MEDIUM_CACHE_MAX + 1};
	for (size_t i = 0; i < ARRAY_SIZE(outside); i++) {
		void *ptr = mallocx(outside[i], 0);
		assert_ptr_not_null(ptr, "Allocation failed");
		dallocx(ptr, 0);
	}
	flush_tcache();
	expect_zu_eq(read_stats().cached_bytes, 0, "Cached size outside range");
	void *ptr = mallocx(SHARED_MEDIUM_CACHE_MAX, MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(ptr, "Allocation failed");
	dallocx(ptr, MALLOCX_TCACHE_NONE);
	expect_zu_eq(
	    read_stats().cached_bytes, 0, "TCACHE_NONE used shared cache");

	unsigned manual;
	size_t   len = sizeof(manual);
	assert_d_eq(mallctl("arenas.create", &manual, &len, NULL, 0), 0,
	    "Create failed");
	ptr = mallocx(SHARED_MEDIUM_CACHE_MAX,
	    MALLOCX_ARENA(manual) | MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(ptr, "Manual arena allocation failed");
	dallocx(ptr, 0);
	expect_zu_eq(
	    read_stats().cached_bytes, 0, "Retained manual arena object");
	char key[64];
	malloc_snprintf(key, sizeof(key), "arena.%u.destroy", manual);
	expect_d_eq(mallctl(key, NULL, NULL, NULL, 0), 0,
	    "Manual arena destroy failed");
}
TEST_END

TEST_BEGIN(test_background_trim) {
	test_skip_if(!shared_medium_cache_active || !have_background_thread);
#if defined(JEMALLOC_BACKGROUND_THREAD)
	flush_tcache();
	flush_shared();
	void *ptr = mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
	assert_ptr_not_null(ptr, "Allocation failed");
	dallocx(ptr, 0);
	bool   enabled = true, was_enabled;
	size_t len = sizeof(was_enabled);
	assert_d_eq(mallctl("background_thread", &was_enabled, &len, &enabled,
	                sizeof(enabled)),
	    0, "Cannot enable background threads");
	/* Two one-second windows: first observe, then release cold inventory. */
	for (unsigned i = 0; i < 5 && read_stats().cached_bytes != 0; i++) {
		sleep(1);
	}
	expect_zu_eq(read_stats().cached_bytes, 0,
	    "Background trim did not release cold object");
	expect_d_eq(mallctl("background_thread", NULL, NULL, &was_enabled,
	                sizeof(was_enabled)),
	    0, "Cannot restore background thread setting");
#endif
}
TEST_END

static void *
allocate_on_arena_one(void *unused) {
	(void)unused;
	bind_arena(1);
	return mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
}

TEST_BEGIN(test_group_and_owner) {
	test_skip_if(!shared_medium_cache_active);
	flush_tcache();
	flush_shared();
	thd_t thread;
	thd_create(&thread, allocate_on_arena_one, NULL);
	void *ptr;
	thd_join(thread, &ptr);
	assert_ptr_not_null(ptr, "Allocation failed");
	edata_t *edata = emap_edata_lookup(
	    tsdn_fetch(), &arena_emap_global, ptr);
	expect_u_eq(edata_arena_ind_get(edata), 1, "Wrong initial arena");
	unsigned pai = edata_pai_get(edata);
	bind_arena(0);
	dallocx(ptr, 0);
	void *reused = mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
	assert_ptr_not_null(reused, "Allocation failed");
	if (opt_experimental_shared_cache_arena_group == 1) {
		expect_ptr_ne(reused, ptr, "Reused across separate groups");
	} else {
		expect_ptr_eq(reused, ptr, "Did not share within arena group");
		edata = emap_edata_lookup(
		    tsdn_fetch(), &arena_emap_global, reused);
		expect_u_eq(
		    edata_arena_ind_get(edata), 1, "Original arena lost");
		expect_u_eq(edata_pai_get(edata), pai, "Original PAI lost");
	}
	dallocx(reused, 0);
	flush_shared();
}
TEST_END

TEST_BEGIN(test_cold_trim) {
	test_skip_if(!shared_medium_cache_active);
	flush_tcache();
	flush_shared();
	void *ptr = mallocx(SHARED_MEDIUM_CACHE_MAX, 0);
	assert_ptr_not_null(ptr, "Allocation failed");
	dallocx(ptr, 0);
	shared_medium_cache_trim(tsd_fetch());
	expect_zu_eq(read_stats().cached_bytes, SHARED_MEDIUM_CACHE_MAX,
	    "Trim discarded freshly cached object");
	shared_medium_cache_trim(tsd_fetch());
	expect_zu_eq(
	    read_stats().cached_bytes, 0, "Cold object was not trimmed");
}
TEST_END

static void *
reuse_worker(void *unused) {
	(void)unused;
	for (unsigned i = 0; i < 200; i++) {
		void *ptr = mallocx(SHARED_MEDIUM_CACHE_MAX, MALLOCX_ZERO);
		assert_ptr_not_null(ptr, "Allocation failed");
		assert_u_eq(((unsigned char *)ptr)[0], 0,
		    "Zeroing lost under contention");
		memset(ptr, 0xa5, SHARED_MEDIUM_CACHE_MAX);
		dallocx(ptr, 0);
		if (i % 16 == 0) {
			flush_shared();
		}
	}
	return NULL;
}

TEST_BEGIN(test_concurrent_flush_and_fork) {
	test_skip_if(!shared_medium_cache_active);
	thd_t threads[2];
	for (unsigned i = 0; i < ARRAY_SIZE(threads); i++) {
		thd_create(&threads[i], reuse_worker, NULL);
	}
#ifndef _WIN32
	pid_t pid = fork();
	assert_d_ne(pid, -1, "Fork failed");
	if (pid == 0) {
		reuse_worker(NULL);
		_exit(0);
	}
	int status;
	assert_d_eq(waitpid(pid, &status, 0), pid, "waitpid failed");
	expect_true(
	    WIFEXITED(status) && WEXITSTATUS(status) == 0, "Child failed");
#endif
	for (unsigned i = 0; i < ARRAY_SIZE(threads); i++) {
		thd_join(threads[i], NULL);
	}
	flush_tcache();
	flush_shared();
	expect_zu_eq(
	    read_stats().cached_bytes, 0, "Concurrent flush left objects");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_reuse_and_zero, test_request_accounting,
	    test_bounds_and_bypasses, test_double_free, test_group_and_owner,
	    test_cold_trim, test_background_trim,
	    test_concurrent_flush_and_fork);
}
