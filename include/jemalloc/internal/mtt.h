#ifndef JEMALLOC_INTERNAL_MTT_H
#define JEMALLOC_INTERNAL_MTT_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/prng.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/tsd.h"

#define MTT_TAG_SHIFT 57
#define MTT_TAG_MASK (UINT64_C(0xf) << MTT_TAG_SHIFT)
#define MTT_GRANULE ZU(16)
#define MTT_FREE_TAG 0

extern bool mtt_active;
extern uintptr_t mtt_tag_table_base;

bool mtt_boot(void);

JEMALLOC_ALWAYS_INLINE size_t
mtt_size_adjust(size_t size) {
	return config_experimental_mtt && unlikely(mtt_active && size < MTT_GRANULE)
	    ? MTT_GRANULE
	    : size;
}

JEMALLOC_ALWAYS_INLINE unsigned
mtt_pointer_tag(const void *ptr) {
	return (unsigned)(((uintptr_t)ptr & MTT_TAG_MASK) >> MTT_TAG_SHIFT);
}

JEMALLOC_ALWAYS_INLINE void *
mtt_untag_pointer(const void *ptr) {
	return (void *)((uintptr_t)ptr & ~((uintptr_t)MTT_TAG_MASK));
}

JEMALLOC_ALWAYS_INLINE void *
mtt_tag_pointer(const void *ptr, unsigned tag) {
	assert(tag <= 0xf);
	return (void *)((uintptr_t)mtt_untag_pointer(ptr)
	    | ((uintptr_t)tag << MTT_TAG_SHIFT));
}

JEMALLOC_ALWAYS_INLINE atomic_u8_t *
mtt_tag_byte(const void *raw_ptr) {
	uintptr_t raw = (uintptr_t)raw_ptr;
	assert((raw & MTT_TAG_MASK) == 0);
	assert((raw >> 63) == 0);
	return (atomic_u8_t *)(mtt_tag_table_base + (raw >> 5));
}

JEMALLOC_ALWAYS_INLINE unsigned
mtt_memory_tag(const void *raw_ptr) {
	uintptr_t raw = (uintptr_t)raw_ptr;
	uint8_t byte = atomic_load_u8(mtt_tag_byte(raw_ptr), ATOMIC_RELAXED);
	return (byte >> ((raw & MTT_GRANULE) == 0 ? 0 : 4)) & 0xf;
}

void mtt_set_tag_range(void *raw_ptr, size_t size, unsigned tag);

JEMALLOC_ALWAYS_INLINE unsigned
mtt_choose_tag(tsdn_t *tsdn, unsigned exclude) {
	assert(exclude <= 0xf);
	if (!tsdn_null(tsdn)) {
		uint64_t *state = tsd_prng_statep_get(tsdn_tsd(tsdn));
		unsigned tag = 1 + (unsigned)prng_range_u64(
		    state, exclude == MTT_FREE_TAG ? 15 : 14);
		if (exclude != 0 && tag >= exclude) {
			tag++;
		}
		return tag;
	}
	return exclude == 0 || exclude == 0xf ? 1 : exclude + 1;
}

JEMALLOC_ALWAYS_INLINE void *
mtt_prepare_allocation(tsdn_t *tsdn, void *raw_ptr, size_t usize) {
	if (!config_experimental_mtt || likely(!mtt_active) || raw_ptr == NULL) {
		return raw_ptr;
	}
	assert(((uintptr_t)raw_ptr & (MTT_GRANULE - 1)) == 0);
	assert((usize & (MTT_GRANULE - 1)) == 0);
	unsigned tag = mtt_memory_tag(raw_ptr);
	if (unlikely(tag == MTT_FREE_TAG)) {
		tag = mtt_choose_tag(tsdn, MTT_FREE_TAG);
		mtt_set_tag_range(raw_ptr, usize, tag);
	}
	return mtt_tag_pointer(raw_ptr, tag);
}

JEMALLOC_ALWAYS_INLINE bool
mtt_decode_pointer(const void *ptr, void **raw_out, unsigned *tag_out) {
	if (!config_experimental_mtt || likely(!mtt_active) || ptr == NULL) {
		*raw_out = (void *)ptr;
		if (tag_out != NULL) {
			*tag_out = 0;
		}
		return false;
	}
	unsigned tag = mtt_pointer_tag(ptr);
	void *raw = mtt_untag_pointer(ptr);
	if (unlikely(tag == MTT_FREE_TAG || mtt_memory_tag(raw) != tag)) {
		safety_check_fail("Invalid MTT-tagged pointer: %p\n", ptr);
		return true;
	}
	*raw_out = raw;
	if (tag_out != NULL) {
		*tag_out = tag;
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE void
mtt_prepare_deallocation(tsdn_t *tsdn, void *raw_ptr, size_t usize,
	bool reusable) {
	if (!config_experimental_mtt || likely(!mtt_active)) {
		return;
	}
	unsigned old_tag = mtt_memory_tag(raw_ptr);
	unsigned new_tag = reusable ? mtt_choose_tag(tsdn, old_tag) : MTT_FREE_TAG;
	mtt_set_tag_range(raw_ptr, usize, new_tag);
}

#endif /* JEMALLOC_INTERNAL_MTT_H */
