// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Primitives for The Physical Address Space
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#define pr_fmt(fmt) "damon-pa: " fmt

#include <linux/mmu_notifier.h>
#include <linux/hash.h>
#include <linux/page_idle.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>

#include "../internal.h"
#include "prmtv-common.h"

/*
 * Fast-path cache to avoid repeated rmap/page-table walks within a single
 * sampling round.  Fixed-size + linear probe to keep overhead low and avoid
 * allocations on hot paths.
 */
#define DAMON_PA_CACHE_BITS	8	/* 256 entries */
#define DAMON_PA_CACHE_SIZE	(1UL << DAMON_PA_CACHE_BITS)
#define DAMON_PA_CACHE_PROBES	4

struct damon_pa_cache_ent {
	unsigned long	gen;
	unsigned long	pfn;
	unsigned long	page_sz;     /* 0 means 'young' not computed yet */
	bool		accessed;
	bool		mkold_done;
};

static struct damon_pa_cache_ent damon_pa_cache[DAMON_PA_CACHE_SIZE];
static unsigned long damon_pa_cache_gen;

static __always_inline struct damon_pa_cache_ent *damon_pa_cache_lookup(
		unsigned long pfn)
{
	unsigned long idx = hash_long(pfn, DAMON_PA_CACHE_BITS);
	int i;

	for (i = 0; i < DAMON_PA_CACHE_PROBES; i++) {
		struct damon_pa_cache_ent *e =
			&damon_pa_cache[(idx + i) & (DAMON_PA_CACHE_SIZE - 1)];

		if (e->gen != damon_pa_cache_gen)
			continue;
		if (e->pfn == pfn)
			return e;
	}
	return NULL;
}

static __always_inline struct damon_pa_cache_ent *damon_pa_cache_get_slot(
		unsigned long pfn)
{
	unsigned long idx = hash_long(pfn, DAMON_PA_CACHE_BITS);
	int i;

	for (i = 0; i < DAMON_PA_CACHE_PROBES; i++) {
		struct damon_pa_cache_ent *e =
			&damon_pa_cache[(idx + i) & (DAMON_PA_CACHE_SIZE - 1)];

		if (e->gen != damon_pa_cache_gen)
			return e;
	}
	/* No stale slot in probe window; overwrite the first one. */
	return &damon_pa_cache[idx & (DAMON_PA_CACHE_SIZE - 1)];
}

static __always_inline void damon_pa_cache_round_begin(void)
{
	/* Logical clear without touching memory.  Never use gen==0 as active. */
	damon_pa_cache_gen++;
	if (!damon_pa_cache_gen)
		damon_pa_cache_gen = 1;
}

static bool __damon_pa_mkold(struct page *page, struct vm_area_struct *vma,
		unsigned long addr, void *arg)
{
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = addr,
	};

	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte)
			damon_ptep_mkold(pvmw.pte, vma->vm_mm, addr);
		else
			damon_pmdp_mkold(pvmw.pmd, vma->vm_mm, addr);
	}
	return true;
}

static void damon_pa_mkold(unsigned long paddr)
{
	struct page *page = damon_get_page(PHYS_PFN(paddr));
	struct rmap_walk_control rwc = {
		.rmap_one = __damon_pa_mkold,
		.anon_lock = page_lock_anon_vma_read,
	};
	bool need_lock;

	if (!page)
		return;

	if (!page_mapped(page) || !page_rmapping(page)) {
		set_page_idle(page);
		goto out;
	}

	need_lock = !PageAnon(page) || PageKsm(page);
	if (need_lock && !trylock_page(page))
		goto out;

	rmap_walk(page, &rwc);

	if (need_lock)
		unlock_page(page);

out:
	put_page(page);
}

static void __damon_pa_prepare_access_check(struct damon_ctx *ctx,
					    struct damon_region *r)
{
	unsigned long pfn;
	struct damon_pa_cache_ent *e;

	r->sampling_addr = damon_rand(r->ar.start, r->ar.end);
	pfn = PHYS_PFN(r->sampling_addr);

	/* Deduplicate mkold within the round to avoid repeated rmap walks. */
	e = damon_pa_cache_lookup(pfn);
	if (e && e->mkold_done)
		return;

	damon_pa_mkold(r->sampling_addr);

	e = damon_pa_cache_get_slot(pfn);
	e->gen = damon_pa_cache_gen;
	e->pfn = pfn;
	e->page_sz = 0;
	e->accessed = false;
	e->mkold_done = true;
}

static void damon_pa_prepare_access_checks(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;

	damon_pa_cache_round_begin();

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t)
			__damon_pa_prepare_access_check(ctx, r);
	}
}

struct damon_pa_access_chk_result {
	unsigned long page_sz;
	bool accessed;
};

static bool __damon_pa_young(struct page *page, struct vm_area_struct *vma,
		unsigned long addr, void *arg)
{
	struct damon_pa_access_chk_result *result = arg;
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = addr,
	};

	result->accessed = false;
	result->page_sz = PAGE_SIZE;
	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte) {
			result->accessed = pte_young(*pvmw.pte) ||
				!page_is_idle(page) ||
				mmu_notifier_test_young(vma->vm_mm, addr);
		} else {
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
			result->accessed = pmd_young(*pvmw.pmd) ||
				!page_is_idle(page) ||
				mmu_notifier_test_young(vma->vm_mm, addr);
			result->page_sz = ((1UL) << HPAGE_PMD_SHIFT);
#else
			WARN_ON_ONCE(1);
#endif	/* CONFIG_TRANSPARENT_HUGEPAGE */
		}
		if (result->accessed) {
			page_vma_mapped_walk_done(&pvmw);
			break;
		}
	}

	/* If accessed, stop walking */
	return !result->accessed;
}

static bool damon_pa_young(unsigned long paddr, unsigned long *page_sz)
{
	struct page *page = damon_get_page(PHYS_PFN(paddr));
	struct damon_pa_access_chk_result result = {
		.page_sz = PAGE_SIZE,
		.accessed = false,
	};
	struct rmap_walk_control rwc = {
		.arg = &result,
		.rmap_one = __damon_pa_young,
		.anon_lock = page_lock_anon_vma_read,
	};
	bool need_lock;

	if (!page)
		return false;

	if (!page_mapped(page) || !page_rmapping(page)) {
		if (page_is_idle(page))
			result.accessed = false;
		else
			result.accessed = true;
		put_page(page);
		goto out;
	}

	need_lock = !PageAnon(page) || PageKsm(page);
	if (need_lock && !trylock_page(page)) {
		*page_sz = PAGE_SIZE;
		put_page(page);
		return false;
	}

	rmap_walk(page, &rwc);

	if (need_lock)
		unlock_page(page);
	put_page(page);

out:
	*page_sz = result.page_sz;
	return result.accessed;
}

static void __damon_pa_check_access(struct damon_ctx *ctx,
				    struct damon_region *r)
{
	unsigned long pfn = PHYS_PFN(r->sampling_addr);
	struct damon_pa_cache_ent *e;
	unsigned long page_sz = PAGE_SIZE;
	bool accessed;

	/* Fast-path: reuse 'young' result if already computed for this PFN. */
	e = damon_pa_cache_lookup(pfn);
	if (e && e->page_sz) {
		accessed = e->accessed;
		if (accessed)
			r->nr_accesses++;
		return;
	}

	accessed = damon_pa_young(r->sampling_addr, &page_sz);

	/* Cache the computed result for this round. */
	e = damon_pa_cache_get_slot(pfn);
	e->gen = damon_pa_cache_gen;
	e->pfn = pfn;
	e->page_sz = page_sz;
	e->accessed = accessed;
	e->mkold_done = true;

	/*
	 * For huge mappings, also cache the base PFN so other samples within the
	 * same huge page can reuse the result even if their PFNs differ.
	 */
	if (page_sz > PAGE_SIZE) {
		unsigned long npages = page_sz >> PAGE_SHIFT;
		unsigned long base_pfn = pfn & ~(npages - 1);

		e = damon_pa_cache_get_slot(base_pfn);
		e->gen = damon_pa_cache_gen;
		e->pfn = base_pfn;
		e->page_sz = page_sz;
		e->accessed = accessed;
		e->mkold_done = true;
	}

	if (accessed)
		r->nr_accesses++;
}

static unsigned int damon_pa_check_accesses(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;
	unsigned int max_nr_accesses = 0;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t) {
			__damon_pa_check_access(ctx, r);
			max_nr_accesses = max(r->nr_accesses, max_nr_accesses);
		}
	}

	return max_nr_accesses;
}

bool damon_pa_target_valid(void *t)
{
	return true;
}

static unsigned long damon_pa_apply_scheme(struct damon_ctx *ctx,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme)
{
	unsigned long addr, applied;
	LIST_HEAD(page_list);

	if (scheme->action != DAMOS_PAGEOUT)
		return 0;

	for (addr = r->ar.start; addr < r->ar.end; addr += PAGE_SIZE) {
		struct page *page = damon_get_page(PHYS_PFN(addr));

		if (!page)
			continue;

		ClearPageReferenced(page);
		test_and_clear_page_young(page);
		if (isolate_lru_page(page)) {
			put_page(page);
			continue;
		}
		if (PageUnevictable(page)) {
			putback_lru_page(page);
		} else {
			list_add(&page->lru, &page_list);
			put_page(page);
		}
	}
	applied = reclaim_pages(&page_list);
	cond_resched();
	return applied * PAGE_SIZE;
}

static int damon_pa_scheme_score(struct damon_ctx *context,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme)
{
	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		return damon_pageout_score(context, r, scheme);
	default:
		break;
	}

	return DAMOS_MAX_SCORE;
}

void damon_pa_set_primitives(struct damon_ctx *ctx)
{
	ctx->primitive.init = NULL;
	ctx->primitive.update = NULL;
	ctx->primitive.prepare_access_checks = damon_pa_prepare_access_checks;
	ctx->primitive.check_accesses = damon_pa_check_accesses;
	ctx->primitive.reset_aggregated = NULL;
	ctx->primitive.target_valid = damon_pa_target_valid;
	ctx->primitive.cleanup = NULL;
	ctx->primitive.apply_scheme = damon_pa_apply_scheme;
	ctx->primitive.get_scheme_score = damon_pa_scheme_score;
}
