#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/arena_inlines.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/witness.h"

bool
bin_update_shard_size(unsigned bin_shard_sizes[SC_NBINS], size_t start_size,
    size_t end_size, size_t nshards) {
	if (nshards > BIN_SHARDS_MAX || nshards == 0) {
		return true;
	}

	if (start_size > SC_SMALL_MAXCLASS) {
		return false;
	}
	if (end_size > SC_SMALL_MAXCLASS) {
		end_size = SC_SMALL_MAXCLASS;
	}

	/* Compute the index since this may happen before sz init. */
	szind_t ind1 = sz_size2index_compute(start_size);
	szind_t ind2 = sz_size2index_compute(end_size);
	for (unsigned i = ind1; i <= ind2; i++) {
		bin_shard_sizes[i] = (unsigned)nshards;
	}

	return false;
}

void
bin_shard_sizes_boot(unsigned bin_shard_sizes[SC_NBINS]) {
	/* Load the default number of shards. */
	for (unsigned i = 0; i < SC_NBINS; i++) {
		bin_shard_sizes[i] = N_BIN_SHARDS_DEFAULT;
	}
}

bool
bin_init(bin_t *bin) {
	if (malloc_mutex_init(&bin->lock, "bin", WITNESS_RANK_BIN,
	        malloc_mutex_rank_exclusive)) {
		return true;
	}
	bin->slabcur = NULL;
	for (unsigned i = 0; i < BIN_SLABSET_NBINS; i++) {
		edata_list_active_init(&bin->slabs_nonfull.dense[i]);
	}
	fb_init(bin->slabs_nonfull.dense_bitmap, BIN_SLABSET_NBINS);
	edata_list_active_init(&bin->slabs_full);
	if (config_stats) {
		memset(&bin->stats, 0, sizeof(bin_stats_t));
	}
	return false;
}

void
bin_prefork(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_prefork(tsdn, &bin->lock);
}

void
bin_postfork_parent(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_postfork_parent(tsdn, &bin->lock);
}

void
bin_postfork_child(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_postfork_child(tsdn, &bin->lock);
}

JET_EXTERN void *
bin_slab_reg_alloc(edata_t *slab, const bin_info_t *bin_info) {
	void        *ret;
	slab_data_t *slab_data = edata_slab_data_get(slab);
	size_t       regind;

	assert(edata_nfree_get(slab) > 0);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

	regind = bitmap_sfu(slab_data->bitmap, &bin_info->bitmap_info);
	ret = (void *)((byte_t *)edata_addr_get(slab)
	    + (uintptr_t)(bin_info->reg_size * regind));
	edata_nfree_dec(slab);
	return ret;
}

void
bin_slab_reg_alloc_batch(
    edata_t *slab, const bin_info_t *bin_info, unsigned cnt, void **ptrs) {
	slab_data_t *slab_data = edata_slab_data_get(slab);

	assert(edata_nfree_get(slab) >= cnt);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

#if (!defined JEMALLOC_INTERNAL_POPCOUNTL) || (defined BITMAP_USE_TREE)
	for (unsigned i = 0; i < cnt; i++) {
		size_t regind = bitmap_sfu(
		    slab_data->bitmap, &bin_info->bitmap_info);
		*(ptrs + i) = (void *)((uintptr_t)edata_addr_get(slab)
		    + (uintptr_t)(bin_info->reg_size * regind));
	}
#else
	unsigned group = 0;
	bitmap_t g = slab_data->bitmap[group];
	unsigned i = 0;
	while (i < cnt) {
		while (g == 0) {
			g = slab_data->bitmap[++group];
		}
		size_t shift = group << LG_BITMAP_GROUP_NBITS;
		size_t pop = popcount_lu(g);
		if (pop > (cnt - i)) {
			pop = cnt - i;
		}

		/*
		 * Load from memory locations only once, outside the
		 * hot loop below.
		 */
		uintptr_t base = (uintptr_t)edata_addr_get(slab);
		uintptr_t regsize = (uintptr_t)bin_info->reg_size;
		while (pop--) {
			size_t bit = cfs_lu(&g);
			size_t regind = shift + bit;
			/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
			*(ptrs + i) = (void *)(base + regsize * regind);

			i++;
		}
		slab_data->bitmap[group] = g;
	}
#endif
	edata_nfree_sub(slab, cnt);
}

static unsigned
bin_slabset_dense_ind(edata_t *slab) {
	szind_t           binind = edata_szind_get(slab);
	const bin_info_t *bin_info = &bin_infos[binind];
	unsigned          nfree = edata_nfree_get(slab);
	assert(nfree > 0);
	assert(nfree <= bin_info->nregs);

	unsigned nactive = bin_info->nregs - nfree;
	/* Use 0, 1, ..., 6, 7+; lower indices are picked first. */
	if (nactive >= BIN_SLABSET_NBINS - 1) {
		return 0;
	}
	return BIN_SLABSET_NBINS - 1 - nactive;
}

static void
bin_slabset_dense_insert(bin_slabset_t *slabset, edata_t *slab) {
	unsigned ind = bin_slabset_dense_ind(slab);
	if (edata_list_active_empty(&slabset->dense[ind])) {
		fb_set(slabset->dense_bitmap, BIN_SLABSET_NBINS, ind);
	}
	edata_list_active_prepend(&slabset->dense[ind], slab);
}

static void
bin_slabset_dense_remove(bin_slabset_t *slabset, edata_t *slab) {
	unsigned ind = bin_slabset_dense_ind(slab);
	edata_list_active_remove(&slabset->dense[ind], slab);
	if (edata_list_active_empty(&slabset->dense[ind])) {
		fb_unset(slabset->dense_bitmap, BIN_SLABSET_NBINS, ind);
	}
}

static edata_t *
bin_slabset_dense_first(bin_slabset_t *slabset) {
	size_t ind = fb_ffs(
	    slabset->dense_bitmap, BIN_SLABSET_NBINS, (size_t)0);
	if (ind == BIN_SLABSET_NBINS) {
		return NULL;
	}
	return edata_list_active_first(&slabset->dense[ind]);
}

static edata_t *
bin_slabset_dense_remove_first(bin_slabset_t *slabset) {
	size_t ind = fb_ffs(
	    slabset->dense_bitmap, BIN_SLABSET_NBINS, (size_t)0);
	if (ind == BIN_SLABSET_NBINS) {
		return NULL;
	}
	edata_t *slab = edata_list_active_first(&slabset->dense[ind]);
	edata_list_active_remove(&slabset->dense[ind], slab);
	if (edata_list_active_empty(&slabset->dense[ind])) {
		fb_unset(slabset->dense_bitmap, BIN_SLABSET_NBINS, ind);
	}
	return slab;
}

void
bin_slabs_nonfull_insert(bin_t *bin, edata_t *slab) {
	assert(edata_nfree_get(slab) > 0);
	bin_slabset_dense_insert(&bin->slabs_nonfull, slab);
	if (config_stats) {
		bin->stats.nonfull_slabs++;
	}
}

void
bin_slabs_nonfull_remove(bin_t *bin, edata_t *slab) {
	bin_slabset_dense_remove(&bin->slabs_nonfull, slab);
	if (config_stats) {
		bin->stats.nonfull_slabs--;
	}
}

edata_t *
bin_slabs_nonfull_remove_first(bin_t *bin) {
	edata_t *slab = bin_slabset_dense_remove_first(&bin->slabs_nonfull);
	if (slab == NULL) {
		return NULL;
	}
	if (config_stats) {
		bin->stats.nonfull_slabs--;
	}
	return slab;
}

edata_t *
bin_slabs_nonfull_tryget(bin_t *bin) {
	edata_t *slab = bin_slabs_nonfull_remove_first(bin);
	if (slab != NULL && config_stats) {
		bin->stats.reslabs++;
	}
	return slab;
}

edata_t *
bin_slabs_nonfull_first(bin_t *bin) {
	return bin_slabset_dense_first(&bin->slabs_nonfull);
}

bool
bin_slabs_nonfull_empty(bin_t *bin) {
	return fb_empty(bin->slabs_nonfull.dense_bitmap, BIN_SLABSET_NBINS);
}

JET_EXTERN void
bin_slabs_full_insert(bool is_auto, bin_t *bin, edata_t *slab) {
	assert(edata_nfree_get(slab) == 0);
	/*
	 *  Tracking extents is required by arena_reset, which is not allowed
	 *  for auto arenas.  Bypass this step to avoid touching the edata
	 *  linkage (often results in cache misses) for auto arenas.
	 */
	if (is_auto) {
		return;
	}
	edata_list_active_append(&bin->slabs_full, slab);
}

void
bin_slabs_full_remove(bool is_auto, bin_t *bin, edata_t *slab) {
	if (is_auto) {
		return;
	}
	edata_list_active_remove(&bin->slabs_full, slab);
}

JET_EXTERN void
bin_dissociate_slab(bool is_auto, edata_t *slab, bin_t *bin) {
	/* Dissociate slab from bin. */
	if (slab == bin->slabcur) {
		bin->slabcur = NULL;
	} else {
		szind_t           binind = edata_szind_get(slab);
		const bin_info_t *bin_info = &bin_infos[binind];

		/*
		 * The following block's conditional is necessary because if the
		 * slab only contains one region, then it never gets inserted
		 * into the non-full slabs heap.
		 */
		if (bin_info->nregs == 1) {
			bin_slabs_full_remove(is_auto, bin, slab);
		} else {
			bin_slabs_nonfull_remove(bin, slab);
		}
	}
}

JET_EXTERN void
bin_lower_slab(tsdn_t *tsdn, bool is_auto, edata_t *slab, bin_t *bin) {
	assert(edata_nfree_get(slab) > 0);

	if (bin->slabcur != NULL && edata_nfree_get(bin->slabcur) > 0) {
		bin_slabs_nonfull_insert(bin, slab);
		return;
	}
	if (bin->slabcur != NULL) {
		bin_slabs_full_insert(is_auto, bin, bin->slabcur);
	}
	bin->slabcur = slab;
	if (config_stats) {
		bin->stats.reslabs++;
	}
}

void
bin_dalloc_slab_prepare(tsdn_t *tsdn, edata_t *slab, bin_t *bin) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);

	assert(slab != bin->slabcur);
	if (config_stats) {
		bin->stats.curslabs--;
	}
}

void
bin_dalloc_locked_handle_newly_empty(
    tsdn_t *tsdn, bool is_auto, edata_t *slab, bin_t *bin) {
	bin_dissociate_slab(is_auto, slab, bin);
	bin_dalloc_slab_prepare(tsdn, slab, bin);
}

void
bin_dalloc_locked_handle_newly_nonempty(
    tsdn_t *tsdn, bool is_auto, edata_t *slab, bin_t *bin) {
	bin_slabs_full_remove(is_auto, bin, slab);
	bin_lower_slab(tsdn, is_auto, slab, bin);
}

void
bin_refill_slabcur_with_fresh_slab(tsdn_t *tsdn, bin_t *bin,
    szind_t binind, edata_t *fresh_slab) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	/* Only called after slabcur and nonfull both failed. */
	assert(bin->slabcur == NULL);
	assert(bin_slabs_nonfull_empty(bin));
	assert(fresh_slab != NULL);

	/* A new slab from arena_slab_alloc() */
	assert(edata_nfree_get(fresh_slab) == bin_infos[binind].nregs);
	if (config_stats) {
		bin->stats.nslabs++;
		bin->stats.curslabs++;
	}
	bin->slabcur = fresh_slab;
}

void *
bin_malloc_with_fresh_slab(tsdn_t *tsdn, bin_t *bin,
    szind_t binind, edata_t *fresh_slab) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	bin_refill_slabcur_with_fresh_slab(tsdn, bin, binind, fresh_slab);

	return bin_slab_reg_alloc(bin->slabcur, &bin_infos[binind]);
}

bool
bin_refill_slabcur_no_fresh_slab(tsdn_t *tsdn, bool is_auto, bin_t *bin) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	/* Only called after bin_slab_reg_alloc[_batch] failed. */
	assert(bin->slabcur == NULL || edata_nfree_get(bin->slabcur) == 0);

	if (bin->slabcur != NULL) {
		bin_slabs_full_insert(is_auto, bin, bin->slabcur);
	}

	/* Look for a usable slab. */
	bin->slabcur = bin_slabs_nonfull_tryget(bin);
	assert(bin->slabcur == NULL || edata_nfree_get(bin->slabcur) > 0);

	return (bin->slabcur == NULL);
}

void *
bin_malloc_no_fresh_slab(tsdn_t *tsdn, bool is_auto, bin_t *bin,
    szind_t binind) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	if (bin->slabcur == NULL || edata_nfree_get(bin->slabcur) == 0) {
		if (bin_refill_slabcur_no_fresh_slab(tsdn, is_auto, bin)) {
			return NULL;
		}
	}

	assert(bin->slabcur != NULL && edata_nfree_get(bin->slabcur) > 0);
	return bin_slab_reg_alloc(bin->slabcur, &bin_infos[binind]);
}

bin_t *
bin_choose(tsdn_t *tsdn, arena_t *arena, szind_t binind,
    unsigned *binshard_p) {
	unsigned binshard;
	if (tsdn_null(tsdn) || tsd_arena_get(tsdn_tsd(tsdn)) == NULL) {
		binshard = 0;
	} else {
		binshard = tsd_binshardsp_get(tsdn_tsd(tsdn))->binshard[binind];
	}
	assert(binshard < bin_infos[binind].n_shards);
	if (binshard_p != NULL) {
		*binshard_p = binshard;
	}
	return arena_get_bin(arena, binind, binshard);
}

void *
bin_current_slab_addr(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_lock(tsdn, &bin->lock);
	edata_t *slab = (bin->slabcur != NULL)
	    ? bin->slabcur
	    : bin_slabs_nonfull_first(bin);
	assert(slab != NULL || bin_slabs_nonfull_empty(bin));
	void *ret = (slab != NULL) ? edata_addr_get(slab) : NULL;
	assert(ret != NULL || slab == NULL);
	malloc_mutex_unlock(tsdn, &bin->lock);
	return ret;
}
