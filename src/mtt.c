#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mtt.h"

bool opt_experimental_mtt = false;
bool mtt_active = false;
uintptr_t mtt_tag_table_base = 0;

JEMALLOC_EXPORT bool
je_experimental_mtt_boot(uintptr_t *tag_table_base, size_t *tag_granule)
#ifndef _WIN32
    JEMALLOC_ATTR(weak)
#endif
    ;

JEMALLOC_EXPORT bool
je_experimental_mtt_boot(uintptr_t *tag_table_base, size_t *tag_granule) {
	(void)tag_table_base;
	(void)tag_granule;
	return true;
}

bool
mtt_boot(void) {
	if (!config_experimental_mtt || !opt_experimental_mtt) {
		return false;
	}
	uintptr_t table_base = 0;
	size_t granule = 0;
	if (je_experimental_mtt_boot(&table_base, &granule)
	    || table_base == 0 || granule != MTT_GRANULE) {
		malloc_write("<jemalloc>: experimental MTT platform boot failed\n");
		return true;
	}
	mtt_tag_table_base = table_base;
	mtt_active = true;
	return false;
}

static void
mtt_set_partial(atomic_u8_t *byte, uint8_t mask, uint8_t value) {
	uint8_t old = atomic_load_u8(byte, ATOMIC_RELAXED);
	do {
		uint8_t desired = (uint8_t)((old & ~mask) | value);
		if (atomic_compare_exchange_weak_u8(byte, &old, desired,
		        ATOMIC_RELAXED, ATOMIC_RELAXED)) {
			return;
		}
	} while (true);
}

void
mtt_set_tag_range(void *raw_ptr, size_t size, unsigned tag) {
	assert(tag <= 0xf);
	assert(((uintptr_t)raw_ptr & (MTT_GRANULE - 1)) == 0);
	assert(size != 0 && (size & (MTT_GRANULE - 1)) == 0);

	uintptr_t raw = (uintptr_t)raw_ptr;
	size_t ngranules = size / MTT_GRANULE;
	atomic_u8_t *byte = mtt_tag_byte(raw_ptr);
	if ((raw & MTT_GRANULE) != 0) {
		mtt_set_partial(byte, UINT8_C(0xf0), (uint8_t)(tag << 4));
		byte++;
		ngranules--;
	}

	uint8_t pair = (uint8_t)(tag | (tag << 4));
	while (ngranules >= 2) {
		atomic_store_u8(byte++, pair, ATOMIC_RELAXED);
		ngranules -= 2;
	}
	if (ngranules != 0) {
		mtt_set_partial(byte, UINT8_C(0x0f), (uint8_t)tag);
	}
}
