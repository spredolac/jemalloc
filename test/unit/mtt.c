#include "test/jemalloc_test.h"

static void
mtt_test_map(void *data, uint8_t *table) {
	mtt_tag_table_base = (uintptr_t)table - ((uintptr_t)data >> 5);
}

TEST_BEGIN(test_mtt_pointer_bits) {
	uintptr_t raw = UINT64_C(0x0000123456789000);
	void *tagged = mtt_tag_pointer((void *)raw, 0xa);
	expect_u_eq(mtt_pointer_tag(tagged), 0xa, "Wrong pointer tag");
	expect_ptr_eq(mtt_untag_pointer(tagged), (void *)raw,
	    "Untagging changed address bits");
}
TEST_END

typedef struct {
	void *ptr;
} mtt_thread_arg_t;

static void *
mtt_update_thread(void *opaque) {
	mtt_thread_arg_t *arg = (mtt_thread_arg_t *)opaque;
	for (unsigned i = 1; i <= 100000; i++) {
		mtt_set_tag_range(arg->ptr, MTT_GRANULE, 1 + (i % 15));
	}
	return NULL;
}

TEST_BEGIN(test_mtt_adjacent_concurrency) {
	uint8_t storage[64];
	void *data = ALIGNMENT_ADDR2CEILING(storage, MTT_GRANULE);
	uint8_t table[4] = {0};
	mtt_test_map(data, table);
	mtt_thread_arg_t args[2] = {
	    {data}, {(byte_t *)data + MTT_GRANULE}};
	thd_t threads[2];
	for (unsigned i = 0; i < 2; i++) {
		thd_create(&threads[i], mtt_update_thread, &args[i]);
	}
	for (unsigned i = 0; i < 2; i++) {
		thd_join(threads[i], NULL);
	}
	unsigned expected = 1 + (100000 % 15);
	expect_u_eq(mtt_memory_tag(args[0].ptr), expected,
	    "Concurrent low-nibble update was lost");
	expect_u_eq(mtt_memory_tag(args[1].ptr), expected,
	    "Concurrent high-nibble update was lost");
}
TEST_END

TEST_BEGIN(test_mtt_nibble_updates) {
	uint8_t storage[96];
	void *data = ALIGNMENT_ADDR2CEILING(storage, MTT_GRANULE);
	uint8_t table[8] = {0};
	mtt_test_map(data, table);

	mtt_set_tag_range(data, MTT_GRANULE, 3);
	mtt_set_tag_range((byte_t *)data + MTT_GRANULE, MTT_GRANULE, 5);
	expect_u_eq(mtt_memory_tag(data), 3, "Low nibble changed");
	expect_u_eq(mtt_memory_tag((byte_t *)data + MTT_GRANULE), 5,
	    "High nibble changed");

	mtt_set_tag_range((byte_t *)data + MTT_GRANULE,
	    3 * MTT_GRANULE, 7);
	expect_u_eq(mtt_memory_tag(data), 3,
	    "Boundary update damaged neighboring granule");
	for (unsigned i = 1; i < 4; i++) {
		expect_u_eq(mtt_memory_tag((byte_t *)data + i * MTT_GRANULE), 7,
		    "Range tag mismatch at granule %u", i);
	}
	expect_u_eq(mtt_memory_tag((byte_t *)data + 4 * MTT_GRANULE), 0,
	    "Range update crossed its end boundary");
}
TEST_END

TEST_BEGIN(test_mtt_tag_selection) {
	for (unsigned old = 0; old <= 0xf; old++) {
		unsigned next = mtt_choose_tag(TSDN_NULL, old);
		expect_u_ge(next, 1, "Tag zero must remain reserved");
		expect_u_le(next, 0xf, "Tag exceeded four bits");
		if (old != 0) {
			expect_u_ne(next, old, "Deallocation tag repeated");
		}
	}
}
TEST_END

TEST_BEGIN(test_mtt_lifecycle) {
	test_skip_if(!config_experimental_mtt);
	uint8_t storage[64];
	void *data = ALIGNMENT_ADDR2CEILING(storage, MTT_GRANULE);
	uint8_t table[4] = {0};
	mtt_test_map(data, table);

	bool old_active = mtt_active;
	mtt_active = true;
	size_t adjusted = mtt_size_adjust(8);
	void *first = mtt_prepare_allocation(TSDN_NULL, data, MTT_GRANULE);
	unsigned first_tag = mtt_pointer_tag(first);
	mtt_prepare_deallocation(TSDN_NULL, data, MTT_GRANULE,
	    /* reusable */ true);
	unsigned prepared_tag = mtt_memory_tag(data);
	void *reused = mtt_prepare_allocation(TSDN_NULL, data, MTT_GRANULE);
	mtt_active = old_active;

	expect_zu_eq(adjusted, MTT_GRANULE,
	    "Tagged allocations must not share a granule");
	expect_u_ne(first_tag, MTT_FREE_TAG,
	    "Allocation returned the reserved free tag");
	expect_u_ne(prepared_tag, first_tag,
	    "Free did not change the object generation");
	expect_u_eq(mtt_pointer_tag(reused), prepared_tag,
	    "Reuse did not consume the prepared tag");
	expect_ptr_eq(mtt_untag_pointer(reused), data,
	    "Reuse changed the raw address");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_mtt_pointer_bits,
	    test_mtt_nibble_updates, test_mtt_adjacent_concurrency,
	    test_mtt_tag_selection, test_mtt_lifecycle);
}
