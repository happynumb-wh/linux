// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/hugetlb.h>
#include <asm/pgalloc.h>
#include <asm/tlb.h>
#include <asm/fixmap.h>
#include <asm/mtrr.h>

#ifdef CONFIG_DYNAMIC_PHYSICAL_MASK
phys_addr_t physical_mask __ro_after_init = (1ULL << __PHYSICAL_MASK_SHIFT) - 1;
EXPORT_SYMBOL(physical_mask);
#endif

#ifdef CONFIG_HIGHPTE
#define PGTABLE_HIGHMEM __GFP_HIGHMEM
#else
#define PGTABLE_HIGHMEM 0
#endif

#ifndef CONFIG_PARAVIRT
static inline
void paravirt_tlb_remove_table(struct mmu_gather *tlb, void *table)
{
	tlb_remove_page(tlb, table);
}
#endif

gfp_t __userpte_alloc_gfp = GFP_PGTABLE_USER | PGTABLE_HIGHMEM;

pgtable_t pte_alloc_one(struct mm_struct *mm)
{
	return __pte_alloc_one(mm, __userpte_alloc_gfp);
}

static int __init setup_userpte(char *arg)
{
	if (!arg)
		return -EINVAL;

	/*
	 * "userpte=nohigh" disables allocation of user pagetables in
	 * high memory.
	 */
	if (strcmp(arg, "nohigh") == 0)
		__userpte_alloc_gfp &= ~__GFP_HIGHMEM;
	else
		return -EINVAL;
	return 0;
}
early_param("userpte", setup_userpte);

void ___pte_free_tlb(struct mmu_gather *tlb, struct page *pte)
{
	pgtable_pte_page_dtor(pte);
	paravirt_release_pte(page_to_pfn(pte));
	paravirt_tlb_remove_table(tlb, pte);
}

#if CONFIG_PGTABLE_LEVELS > 2
void ___pmd_free_tlb(struct mmu_gather *tlb, pmd_t *pmd)
{
	struct page *page = virt_to_page(pmd);
	paravirt_release_pmd(__pa(pmd) >> PAGE_SHIFT);
	/*
	 * NOTE! For PAE, any changes to the top page-directory-pointer-table
	 * entries need a full cr3 reload to flush.
	 */
#ifdef CONFIG_X86_PAE
	tlb->need_flush_all = 1;
#endif
	pgtable_pmd_page_dtor(page);
	paravirt_tlb_remove_table(tlb, page);
}

#if CONFIG_PGTABLE_LEVELS > 3
void ___pud_free_tlb(struct mmu_gather *tlb, pud_t *pud)
{
	paravirt_release_pud(__pa(pud) >> PAGE_SHIFT);
	paravirt_tlb_remove_table(tlb, virt_to_page(pud));
}

#if CONFIG_PGTABLE_LEVELS > 4
void ___p4d_free_tlb(struct mmu_gather *tlb, p4d_t *p4d)
{
	paravirt_release_p4d(__pa(p4d) >> PAGE_SHIFT);
	paravirt_tlb_remove_table(tlb, virt_to_page(p4d));
}
#endif	/* CONFIG_PGTABLE_LEVELS > 4 */
#endif	/* CONFIG_PGTABLE_LEVELS > 3 */
#endif	/* CONFIG_PGTABLE_LEVELS > 2 */

static inline void pgd_list_add(pgd_t *pgd)
{
	struct page *page = virt_to_page(pgd);

	list_add(&page->lru, &pgd_list);
}

static inline void pgd_list_del(pgd_t *pgd)
{
	struct page *page = virt_to_page(pgd);

	list_del(&page->lru);
}

#define UNSHARED_PTRS_PER_PGD				\
	(SHARED_KERNEL_PMD ? KERNEL_PGD_BOUNDARY : PTRS_PER_PGD)
#define MAX_UNSHARED_PTRS_PER_PGD			\
	max_t(size_t, KERNEL_PGD_BOUNDARY, PTRS_PER_PGD)


static void pgd_set_mm(pgd_t *pgd, struct mm_struct *mm)
{
	virt_to_page(pgd)->pt_mm = mm;
}

struct mm_struct *pgd_page_get_mm(struct page *page)
{
	return page->pt_mm;
}

static void pgd_ctor(struct mm_struct *mm, pgd_t *pgd)
{
	/* If the pgd points to a shared pagetable level (either the
	   ptes in non-PAE, or shared PMD in PAE), then just copy the
	   references from swapper_pg_dir. */
	if (CONFIG_PGTABLE_LEVELS == 2 ||
	    (CONFIG_PGTABLE_LEVELS == 3 && SHARED_KERNEL_PMD) ||
	    CONFIG_PGTABLE_LEVELS >= 4) {
		clone_pgd_range(pgd + KERNEL_PGD_BOUNDARY,
				swapper_pg_dir + KERNEL_PGD_BOUNDARY,
				KERNEL_PGD_PTRS);
	}

	/* list required to sync kernel mapping updates */
	if (!SHARED_KERNEL_PMD) {
		pgd_set_mm(pgd, mm);
		pgd_list_add(pgd);
	}
}

static void pgd_dtor(pgd_t *pgd)
{
	if (SHARED_KERNEL_PMD)
		return;

	spin_lock(&pgd_lock);
	pgd_list_del(pgd);
	spin_unlock(&pgd_lock);
}

/*
 * List of all pgd's needed for non-PAE so it can invalidate entries
 * in both cached and uncached pgd's; not needed for PAE since the
 * kernel pmd is shared. If PAE were not to share the pmd a similar
 * tactic would be needed. This is essentially codepath-based locking
 * against pageattr.c; it is the unique case in which a valid change
 * of kernel pagetables can't be lazily synchronized by vmalloc faults.
 * vmalloc faults work because attached pagetables are never freed.
 * -- nyc
 */

#ifdef CONFIG_X86_PAE
/*
 * In PAE mode, we need to do a cr3 reload (=tlb flush) when
 * updating the top-level pagetable entries to guarantee the
 * processor notices the update.  Since this is expensive, and
 * all 4 top-level entries are used almost immediately in a
 * new process's life, we just pre-populate them here.
 *
 * Also, if we're in a paravirt environment where the kernel pmd is
 * not shared between pagetables (!SHARED_KERNEL_PMDS), we allocate
 * and initialize the kernel pmds here.
 */
#define PREALLOCATED_PMDS	UNSHARED_PTRS_PER_PGD
#define MAX_PREALLOCATED_PMDS	MAX_UNSHARED_PTRS_PER_PGD

/*
 * We allocate separate PMDs for the kernel part of the user page-table
 * when PTI is enabled. We need them to map the per-process LDT into the
 * user-space page-table.
 */
#define PREALLOCATED_USER_PMDS	 (boot_cpu_has(X86_FEATURE_PTI) ? \
					KERNEL_PGD_PTRS : 0)
#define MAX_PREALLOCATED_USER_PMDS KERNEL_PGD_PTRS

void pud_populate(struct mm_struct *mm, pud_t *pudp, pmd_t *pmd)
{
	paravirt_alloc_pmd(mm, __pa(pmd) >> PAGE_SHIFT);

	/* Note: almost everything apart from _PAGE_PRESENT is
	   reserved at the pmd (PDPT) level. */
	set_pud(pudp, __pud(__pa(pmd) | _PAGE_PRESENT));

	/*
	 * According to Intel App note "TLBs, Paging-Structure Caches,
	 * and Their Invalidation", April 2007, document 317080-001,
	 * section 8.1: in PAE mode we explicitly have to flush the
	 * TLB via cr3 if the top-level pgd is changed...
	 */
	flush_tlb_mm(mm);
}
#else  /* !CONFIG_X86_PAE */

/* No need to prepopulate any pagetable entries in non-PAE modes. */
#define PREALLOCATED_PMDS	0
#define MAX_PREALLOCATED_PMDS	0
#define PREALLOCATED_USER_PMDS	 0
#define MAX_PREALLOCATED_USER_PMDS 0
#endif	/* CONFIG_X86_PAE */

static void free_pmds(struct mm_struct *mm, pmd_t *pmds[], int count)
{
	int i;

	for (i = 0; i < count; i++)
		if (pmds[i]) {
			pgtable_pmd_page_dtor(virt_to_page(pmds[i]));
			free_page((unsigned long)pmds[i]);
			mm_dec_nr_pmds(mm);
		}
}

static int preallocate_pmds(struct mm_struct *mm, pmd_t *pmds[], int count)
{
	int i;
	bool failed = false;
	gfp_t gfp = GFP_PGTABLE_USER;

	if (mm == &init_mm)
		gfp &= ~__GFP_ACCOUNT;

	for (i = 0; i < count; i++) {
		pmd_t *pmd = (pmd_t *)__get_free_page(gfp);
		if (!pmd)
			failed = true;
		if (pmd && !pgtable_pmd_page_ctor(virt_to_page(pmd))) {
			free_page((unsigned long)pmd);
			pmd = NULL;
			failed = true;
		}
		if (pmd)
			mm_inc_nr_pmds(mm);
		pmds[i] = pmd;
	}

	if (failed) {
		free_pmds(mm, pmds, count);
		return -ENOMEM;
	}

	return 0;
}

/*
 * Mop up any pmd pages which may still be attached to the pgd.
 * Normally they will be freed by munmap/exit_mmap, but any pmd we
 * preallocate which never got a corresponding vma will need to be
 * freed manually.
 */
static void mop_up_one_pmd(struct mm_struct *mm, pgd_t *pgdp)
{
	pgd_t pgd = *pgdp;

	if (pgd_val(pgd) != 0) {
		pmd_t *pmd = (pmd_t *)pgd_page_vaddr(pgd);

		pgd_clear(pgdp);

		paravirt_release_pmd(pgd_val(pgd) >> PAGE_SHIFT);
		pmd_free(mm, pmd);
		mm_dec_nr_pmds(mm);
	}
}

static void pgd_mop_up_pmds(struct mm_struct *mm, pgd_t *pgdp)
{
	int i;

	for (i = 0; i < PREALLOCATED_PMDS; i++)
		mop_up_one_pmd(mm, &pgdp[i]);

#ifdef CONFIG_PAGE_TABLE_ISOLATION

	if (!boot_cpu_has(X86_FEATURE_PTI))
		return;

	pgdp = kernel_to_user_pgdp(pgdp);

	for (i = 0; i < PREALLOCATED_USER_PMDS; i++)
		mop_up_one_pmd(mm, &pgdp[i + KERNEL_PGD_BOUNDARY]);
#endif
}

static void pgd_prepopulate_pmd(struct mm_struct *mm, pgd_t *pgd, pmd_t *pmds[])
{
	p4d_t *p4d;
	pud_t *pud;
	int i;

	if (PREALLOCATED_PMDS == 0) /* Work around gcc-3.4.x bug */
		return;

	p4d = p4d_offset(pgd, 0);
	pud = pud_offset(p4d, 0);

	for (i = 0; i < PREALLOCATED_PMDS; i++, pud++) {
		pmd_t *pmd = pmds[i];

		if (i >= KERNEL_PGD_BOUNDARY)
			memcpy(pmd, (pmd_t *)pgd_page_vaddr(swapper_pg_dir[i]),
			       sizeof(pmd_t) * PTRS_PER_PMD);

		pud_populate(mm, pud, pmd);
	}
}

#ifdef CONFIG_PAGE_TABLE_ISOLATION
static void pgd_prepopulate_user_pmd(struct mm_struct *mm,
				     pgd_t *k_pgd, pmd_t *pmds[])
{
	pgd_t *s_pgd = kernel_to_user_pgdp(swapper_pg_dir);
	pgd_t *u_pgd = kernel_to_user_pgdp(k_pgd);
	p4d_t *u_p4d;
	pud_t *u_pud;
	int i;

	u_p4d = p4d_offset(u_pgd, 0);
	u_pud = pud_offset(u_p4d, 0);

	s_pgd += KERNEL_PGD_BOUNDARY;
	u_pud += KERNEL_PGD_BOUNDARY;

	for (i = 0; i < PREALLOCATED_USER_PMDS; i++, u_pud++, s_pgd++) {
		pmd_t *pmd = pmds[i];

		memcpy(pmd, (pmd_t *)pgd_page_vaddr(*s_pgd),
		       sizeof(pmd_t) * PTRS_PER_PMD);

		pud_populate(mm, u_pud, pmd);
	}

}
#else
static void pgd_prepopulate_user_pmd(struct mm_struct *mm,
				     pgd_t *k_pgd, pmd_t *pmds[])
{
}
#endif
/*
 * Xen paravirt assumes pgd table should be in one page. 64 bit kernel also
 * assumes that pgd should be in one page.
 *
 * But kernel with PAE paging that is not running as a Xen domain
 * only needs to allocate 32 bytes for pgd instead of one page.
 */
#ifdef CONFIG_X86_PAE

#include <linux/slab.h>

#define PGD_SIZE	(PTRS_PER_PGD * sizeof(pgd_t))
#define PGD_ALIGN	32

static struct kmem_cache *pgd_cache;

void __init pgtable_cache_init(void)
{
	/*
	 * When PAE kernel is running as a Xen domain, it does not use
	 * shared kernel pmd. And this requires a whole page for pgd.
	 */
	if (!SHARED_KERNEL_PMD)
		return;

	/*
	 * when PAE kernel is not running as a Xen domain, it uses
	 * shared kernel pmd. Shared kernel pmd does not require a whole
	 * page for pgd. We are able to just allocate a 32-byte for pgd.
	 * During boot time, we create a 32-byte slab for pgd table allocation.
	 */
	pgd_cache = kmem_cache_create("pgd_cache", PGD_SIZE, PGD_ALIGN,
				      SLAB_PANIC, NULL);
}

static inline pgd_t *_pgd_alloc(void)
{
	/*
	 * If no SHARED_KERNEL_PMD, PAE kernel is running as a Xen domain.
	 * We allocate one page for pgd.
	 */
	if (!SHARED_KERNEL_PMD)
		return (pgd_t *)__get_free_pages(GFP_PGTABLE_USER,
						 PGD_ALLOCATION_ORDER);

	/*
	 * Now PAE kernel is not running as a Xen domain. We can allocate
	 * a 32-byte slab for pgd to save memory space.
	 */
	return kmem_cache_alloc(pgd_cache, GFP_PGTABLE_USER);
}

static inline void _pgd_free(pgd_t *pgd)
{
	if (!SHARED_KERNEL_PMD)
		free_pages((unsigned long)pgd, PGD_ALLOCATION_ORDER);
	else
		kmem_cache_free(pgd_cache, pgd);
}
#else

static inline pgd_t *_pgd_alloc(void)
{
	return (pgd_t *)__get_free_pages(GFP_PGTABLE_USER,
					 PGD_ALLOCATION_ORDER);
}

static inline void _pgd_free(pgd_t *pgd)
{
	free_pages((unsigned long)pgd, PGD_ALLOCATION_ORDER);
}
#endif /* CONFIG_X86_PAE */

pgd_t *pgd_alloc(struct mm_struct *mm)
{
	pgd_t *pgd;
	pmd_t *u_pmds[MAX_PREALLOCATED_USER_PMDS];
	pmd_t *pmds[MAX_PREALLOCATED_PMDS];

	pgd = _pgd_alloc();

	if (pgd == NULL)
		goto out;

	mm->pgd = pgd;

	if (preallocate_pmds(mm, pmds, PREALLOCATED_PMDS) != 0)
		goto out_free_pgd;

	if (preallocate_pmds(mm, u_pmds, PREALLOCATED_USER_PMDS) != 0)
		goto out_free_pmds;

	if (paravirt_pgd_alloc(mm) != 0)
		goto out_free_user_pmds;

	/*
	 * Make sure that pre-populating the pmds is atomic with
	 * respect to anything walking the pgd_list, so that they
	 * never see a partially populated pgd.
	 */
	spin_lock(&pgd_lock);

	pgd_ctor(mm, pgd);
	pgd_prepopulate_pmd(mm, pgd, pmds);
	pgd_prepopulate_user_pmd(mm, pgd, u_pmds);

	spin_unlock(&pgd_lock);

	return pgd;

out_free_user_pmds:
	free_pmds(mm, u_pmds, PREALLOCATED_USER_PMDS);
out_free_pmds:
	free_pmds(mm, pmds, PREALLOCATED_PMDS);
out_free_pgd:
	_pgd_free(pgd);
out:
	return NULL;
}

void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	pgd_mop_up_pmds(mm, pgd);
	pgd_dtor(pgd);
	paravirt_pgd_free(mm, pgd);
	_pgd_free(pgd);
}

/*
 * Used to set accessed or dirty bits in the page table entries
 * on other architectures. On x86, the accessed and dirty bits
 * are tracked by hardware. However, do_wp_page calls this function
 * to also make the pte writeable at the same time the dirty bit is
 * set. In that case we do actually need to write the PTE.
 */
int ptep_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pte_t *ptep,
			  pte_t entry, int dirty)
{
	int changed = !pte_same(*ptep, entry);

	if (changed && dirty)
		set_pte(ptep, entry);

	return changed;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int pmdp_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pmd_t *pmdp,
			  pmd_t entry, int dirty)
{
	int changed = !pmd_same(*pmdp, entry);

	VM_BUG_ON(address & ~HPAGE_PMD_MASK);

	if (changed && dirty) {
		set_pmd(pmdp, entry);
		/*
		 * We had a write-protection fault here and changed the pmd
		 * to to more permissive. No need to flush the TLB for that,
		 * #PF is architecturally guaranteed to do that and in the
		 * worst-case we'll generate a spurious fault.
		 */
	}

	return changed;
}

int pudp_set_access_flags(struct vm_area_struct *vma, unsigned long address,
			  pud_t *pudp, pud_t entry, int dirty)
{
	int changed = !pud_same(*pudp, entry);

	VM_BUG_ON(address & ~HPAGE_PUD_MASK);

	if (changed && dirty) {
		set_pud(pudp, entry);
		/*
		 * We had a write-protection fault here and changed the pud
		 * to to more permissive. No need to flush the TLB for that,
		 * #PF is architecturally guaranteed to do that and in the
		 * worst-case we'll generate a spurious fault.
		 */
	}

	return changed;
}
#endif

int ptep_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pte_t *ptep)
{
	int ret = 0;

	if (pte_young(*ptep))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *) &ptep->pte);

	return ret;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int pmdp_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pmd_t *pmdp)
{
	int ret = 0;

	if (pmd_young(*pmdp))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)pmdp);

	return ret;
}
int pudp_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pud_t *pudp)
{
	int ret = 0;

	if (pud_young(*pudp))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)pudp);

	return ret;
}
#endif

int ptep_clear_flush_young(struct vm_area_struct *vma,
			   unsigned long address, pte_t *ptep)
{
	/*
	 * On x86 CPUs, clearing the accessed bit without a TLB flush
	 * doesn't cause data corruption. [ It could cause incorrect
	 * page aging and the (mistaken) reclaim of hot pages, but the
	 * chance of that should be relatively low. ]
	 *
	 * So as a performance optimization don't flush the TLB when
	 * clearing the accessed bit, it will eventually be flushed by
	 * a context switch or a VM operation anyway. [ In the rare
	 * event of it not getting flushed for a long time the delay
	 * shouldn't really matter because there's no real memory
	 * pressure for swapout to react to. ]
	 */
	return ptep_test_and_clear_young(vma, address, ptep);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
int pmdp_clear_flush_young(struct vm_area_struct *vma,
			   unsigned long address, pmd_t *pmdp)
{
	int young;

	VM_BUG_ON(address & ~HPAGE_PMD_MASK);

	young = pmdp_test_and_clear_young(vma, address, pmdp);
	if (young)
		flush_tlb_range(vma, address, address + HPAGE_PMD_SIZE);

	return young;
}

pmd_t pmdp_invalidate_ad(struct vm_area_struct *vma, unsigned long address,
			 pmd_t *pmdp)
{
	/*
	 * No flush is necessary. Once an invalid PTE is established, the PTE's
	 * access and dirty bits cannot be updated.
	 */
	return pmdp_establish(vma, address, pmdp, pmd_mkinvalid(*pmdp));
}
#endif

/**
 * reserve_top_address - reserves a hole in the top of kernel address space
 * @reserve - size of hole to reserve
 *
 * Can be used to relocate the fixmap area and poke a hole in the top
 * of kernel address space to make room for a hypervisor.
 */
void __init reserve_top_address(unsigned long reserve)
{
#ifdef CONFIG_X86_32
	BUG_ON(fixmaps_set > 0);
	__FIXADDR_TOP = round_down(-reserve, 1 << PMD_SHIFT) - PAGE_SIZE;
	printk(KERN_INFO "Reserving virtual address space above 0x%08lx (rounded to 0x%08lx)\n",
	       -reserve, __FIXADDR_TOP + PAGE_SIZE);
#endif
}

int fixmaps_set;

void __native_set_fixmap(enum fixed_addresses idx, pte_t pte)
{
	unsigned long address = __fix_to_virt(idx);

#ifdef CONFIG_X86_64
       /*
	* Ensure that the static initial page tables are covering the
	* fixmap completely.
	*/
	BUILD_BUG_ON(__end_of_permanent_fixed_addresses >
		     (FIXMAP_PMD_NUM * PTRS_PER_PTE));
#endif

	if (idx >= __end_of_fixed_addresses) {
		BUG();
		return;
	}
	set_pte_vaddr(address, pte);
	fixmaps_set++;
}

void native_set_fixmap(unsigned /* enum fixed_addresses */ idx,
		       phys_addr_t phys, pgprot_t flags)
{
	/* Sanitize 'prot' against any unsupported bits: */
	pgprot_val(flags) &= __default_kernel_pte_mask;

	__native_set_fixmap(idx, pfn_pte(phys >> PAGE_SHIFT, flags));
}

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
#ifdef CONFIG_X86_5LEVEL
/**
 * p4d_set_huge - setup kernel P4D mapping
 *
 * No 512GB pages yet -- always return 0
 */
int p4d_set_huge(p4d_t *p4d, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}

/**
 * p4d_clear_huge - clear kernel P4D mapping when it is set
 *
 * No 512GB pages yet -- always return 0
 */
void p4d_clear_huge(p4d_t *p4d)
{
}
#endif

/**
 * pud_set_huge - setup kernel PUD mapping
 *
 * MTRRs can override PAT memory types with 4KiB granularity. Therefore, this
 * function sets up a huge page only if any of the following conditions are met:
 *
 * - MTRRs are disabled, or
 *
 * - MTRRs are enabled and the range is completely covered by a single MTRR, or
 *
 * - MTRRs are enabled and the corresponding MTRR memory type is WB, which
 *   has no effect on the requested PAT memory type.
 *
 * Callers should try to decrease page size (1GB -> 2MB -> 4K) if the bigger
 * page mapping attempt fails.
 *
 * Returns 1 on success and 0 on failure.
 */
int pud_set_huge(pud_t *pud, phys_addr_t addr, pgprot_t prot)
{
	u8 mtrr, uniform;

	mtrr = mtrr_type_lookup(addr, addr + PUD_SIZE, &uniform);
	if ((mtrr != MTRR_TYPE_INVALID) && (!uniform) &&
	    (mtrr != MTRR_TYPE_WRBACK))
		return 0;

	/* Bail out if we are we on a populated non-leaf entry: */
	if (pud_present(*pud) && !pud_huge(*pud))
		return 0;

	set_pte((pte_t *)pud, pfn_pte(
		(u64)addr >> PAGE_SHIFT,
		__pgprot(protval_4k_2_large(pgprot_val(prot)) | _PAGE_PSE)));

	return 1;
}

/**
 * pmd_set_huge - setup kernel PMD mapping
 *
 * See text over pud_set_huge() above.
 *
 * Returns 1 on success and 0 on failure.
 */
int pmd_set_huge(pmd_t *pmd, phys_addr_t addr, pgprot_t prot)
{
	u8 mtrr, uniform;

	mtrr = mtrr_type_lookup(addr, addr + PMD_SIZE, &uniform);
	if ((mtrr != MTRR_TYPE_INVALID) && (!uniform) &&
	    (mtrr != MTRR_TYPE_WRBACK)) {
		pr_warn_once("%s: Cannot satisfy [mem %#010llx-%#010llx] with a huge-page mapping due to MTRR override.\n",
			     __func__, addr, addr + PMD_SIZE);
		return 0;
	}

	/* Bail out if we are we on a populated non-leaf entry: */
	if (pmd_present(*pmd) && !pmd_huge(*pmd))
		return 0;

	set_pte((pte_t *)pmd, pfn_pte(
		(u64)addr >> PAGE_SHIFT,
		__pgprot(protval_4k_2_large(pgprot_val(prot)) | _PAGE_PSE)));

	return 1;
}

/**
 * pud_clear_huge - clear kernel PUD mapping when it is set
 *
 * Returns 1 on success and 0 on failure (no PUD map is found).
 */
int pud_clear_huge(pud_t *pud)
{
	if (pud_large(*pud)) {
		pud_clear(pud);
		return 1;
	}

	return 0;
}

/**
 * pmd_clear_huge - clear kernel PMD mapping when it is set
 *
 * Returns 1 on success and 0 on failure (no PMD map is found).
 */
int pmd_clear_huge(pmd_t *pmd)
{
	if (pmd_large(*pmd)) {
		pmd_clear(pmd);
		return 1;
	}

	return 0;
}

#ifdef CONFIG_X86_64
/**
 * pud_free_pmd_page - Clear pud entry and free pmd page.
 * @pud: Pointer to a PUD.
 * @addr: Virtual address associated with pud.
 *
 * Context: The pud range has been unmapped and TLB purged.
 * Return: 1 if clearing the entry succeeded. 0 otherwise.
 *
 * NOTE: Callers must allow a single page allocation.
 */
int pud_free_pmd_page(pud_t *pud, unsigned long addr)
{
	pmd_t *pmd, *pmd_sv;
	pte_t *pte;
	int i;

	pmd = pud_pgtable(*pud);
	pmd_sv = (pmd_t *)__get_free_page(GFP_KERNEL);
	if (!pmd_sv)
		return 0;

	for (i = 0; i < PTRS_PER_PMD; i++) {
		pmd_sv[i] = pmd[i];
		if (!pmd_none(pmd[i]))
			pmd_clear(&pmd[i]);
	}

	pud_clear(pud);

	/* INVLPG to clear all paging-structure caches */
	flush_tlb_kernel_range(addr, addr + PAGE_SIZE-1);

	for (i = 0; i < PTRS_PER_PMD; i++) {
		if (!pmd_none(pmd_sv[i])) {
			pte = (pte_t *)pmd_page_vaddr(pmd_sv[i]);
			free_page((unsigned long)pte);
		}
	}

	free_page((unsigned long)pmd_sv);

	pgtable_pmd_page_dtor(virt_to_page(pmd));
	free_page((unsigned long)pmd);

	return 1;
}

/**
 * pmd_free_pte_page - Clear pmd entry and free pte page.
 * @pmd: Pointer to a PMD.
 * @addr: Virtual address associated with pmd.
 *
 * Context: The pmd range has been unmapped and TLB purged.
 * Return: 1 if clearing the entry succeeded. 0 otherwise.
 */
int pmd_free_pte_page(pmd_t *pmd, unsigned long addr)
{
	pte_t *pte;

	pte = (pte_t *)pmd_page_vaddr(*pmd);
	pmd_clear(pmd);

	/* INVLPG to clear all paging-structure caches */
	flush_tlb_kernel_range(addr, addr + PAGE_SIZE-1);

	free_page((unsigned long)pte);

	return 1;
}

#else /* !CONFIG_X86_64 */

/*
 * Disable free page handling on x86-PAE. This assures that ioremap()
 * does not update sync'd pmd entries. See vmalloc_sync_one().
 */
int pmd_free_pte_page(pmd_t *pmd, unsigned long addr)
{
	return pmd_none(*pmd);
}

#endif /* CONFIG_X86_64 */
#endif	/* CONFIG_HAVE_ARCH_HUGE_VMAP */

/************************************************************************************
 *  Printk PageTable Infomation -- Begin
 *
 *  Author: Yungang Bao
 *  Date:   July 10, 2009
 *  Modified By: clc, 22/2/2011
 *               ZCG, 09/10/2023 (dd/mm/yyyy)
 *  Last Edited on 12/12/2023
 *  ********************************************************************************/
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <asm/pgtable_64.h>

#define PAGE_TABLE_DEBUG__
#ifdef PAGE_TABLE_DEBUG__
#include <linux/spinlock.h>
#include <asm/current.h>


//#define PTE_KERNEL_PRINTK__

#define  PAGE_TABLE_TRACE_SIZE  13

char               page_table_val[16];
EXPORT_SYMBOL(page_table_val);


char set_page_table_magic   = 0xec;
char free_page_table_magic  = 0xfc;
char free_page_table_get_clear = 0x22;
char free_page_table_get_clear_full = 0x33;

char set_huge_page_table_magic   = 0x44;   // set_pmd_at(), native_set_pmd()
char free_huge_page_table_magic  = 0x55;   // native_pmd_clear()
char free_huge_page_table_get_clear = 0x66;   // pmdp_huge_get_and_clear()

///////////////////////////////////////////////////////////////////////////////////////////
//we need to monitor the page table physical address to decide whether is the page table memory access
////////////////////////////////////////////////////////////
#define  DUMP_PT_ADDR_TRACE

//##define  PRINTK_PT_ADDR_DEBUG

#ifdef   DUMP_PT_ADDR_TRACE
char set_pt_addr_magic      = 0xed;
char free_pt_addr_magic     = 0xfd;
int pt_addr_trace           = 0;
EXPORT_SYMBOL(pt_addr_trace);
EXPORT_SYMBOL(set_pt_addr_magic);


unsigned long long pt_addr_pgd_flag = 0x08ULL << 48;
unsigned long long pt_addr_pud_flag = 0x04ULL << 48;
unsigned long long pt_addr_pmd_flag = 0x02ULL << 48;
unsigned long long pt_addr_pte_flag = 0x01ULL << 48;

#endif
///////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////


unsigned long long all_set_pte_cnt = 0;
unsigned long long set_pte_at_cnt =  0;
unsigned long long native_set_pte_cnt = 0;

unsigned long long all_pte_clear_cnt = 0;
unsigned long long native_pte_clear_cnt = 0;
unsigned long long ptep_get_and_clear_cnt = 0;
unsigned long long ptep_get_and_clear_full_cnt = 0;

unsigned long long all_set_pmd_cnt = 0;
unsigned long long set_pmd_at_cnt = 0;
unsigned long long native_set_pmd_cnt = 0;

unsigned long long all_pmd_clear_cnt = 0;
unsigned long long native_pmd_clear_cnt = 0;
unsigned long long pmdp_huge_get_and_clear_cnt = 0;


unsigned long long dump_set_pte_cnt = 0;
unsigned long long dump_set_other_cnt = 0;


unsigned int       *pid_trace;
unsigned long long *pte_trace;
unsigned long long *pmd_trace;

EXPORT_SYMBOL(pid_trace);
EXPORT_SYMBOL(pte_trace);
EXPORT_SYMBOL(pmd_trace);

int                page_table_trace = 0;

int page_table_kernel_printk = 0;
int num_pte_kernel_printk = 0;
int max_num_pte_kernel_printk = 10;

 #define E1K_USE_SPINLOCK 
#ifdef E1K_USE_SPINLOCK
/* Modified by Zhang Jiutian, 28/3/2014 */
//spinlock_t e1k_dma_lock = SPIN_LOCK_UNLOCKED;
spinlock_t e1k_dma_lock = __SPIN_LOCK_UNLOCKED();
unsigned long page_table_flags = 0;
extern spinlock_t e1k_dma_lock;
EXPORT_SYMBOL(e1k_dma_lock);
#endif


#define PAGE_TABLE_USE_SPINLOCK 
#ifdef PAGE_TABLE_USE_SPINLOCK
/*********************************************************************************
 * we just dump the page table, omit the dma trace
 * the e1k_dma_lock is used to protect the kernel buf dump op, so it must be used with context switch
 * ******************************************************************************/
//extern spinlock_t  e1k_dma_lock;

//unsigned long      page_table_flags; //what is this pgtbl_flags doing for
#endif

/*******************************************************************************
 * There functions are used to printk some help message in kernel log messages
 * ****************************************************************************/
void page_table_print_start(void)
{
    printk("**********************************************\n");
}

void page_table_print_end(void)
{
    printk("**********************************************\n\n");
}


#ifdef   DUMP_PT_ADDR_TRACE
int pt_addr_start_trace(void)
{
        pt_addr_trace = 1;
        page_table_print_start();
        printk("<1>pt_addr_start_trace!\n");
        page_table_print_end();

        return 0;
}
int pt_addr_stop_trace(void)
{
        pt_addr_trace = 0;
        page_table_print_start();
        printk("<1>pt_addr_stop_trace!\n");
        page_table_print_end();

        return 0;
}
#endif
int page_table_start_trace(void)
{

    page_table_trace = 1;

    page_table_print_start();
    printk("page_table_start_trace!\n");
    page_table_print_end();

    return 0;
}

int page_table_stop_trace(void)
{
    page_table_trace = 0;

    page_table_print_start();
    printk("page_table_stop_trace!\n");
    page_table_print_end();

    return 0;
}
int page_table_set_kernel_printk(void)
{
        page_table_kernel_printk = 1;
        num_pte_kernel_printk = 0;
        return 0;
}

int page_table_reset_kernel_printk(void)
{
        page_table_kernel_printk = 0;
        num_pte_kernel_printk = 0;
        return 0;
}

//this must be called at the exit of dump trace process, show all 
int page_table_stop_and_clear_trace(void)
{
    page_table_print_start();
    printk("page_table_stop_and_clear_trace!\n");

    page_table_trace = 0;
    printk("all_set_pte_cnt  [%llu]\n",all_set_pte_cnt);
    printk("set_pte_at_cnt [%llu]\n",set_pte_at_cnt);
	printk("native_set_pte_cnt [%llu]\n",native_set_pte_cnt);

    printk("all_pte_clear_cnt [%llu]\n",all_pte_clear_cnt);
	printk("native_pte_clear_cnt [%llu]\n",native_pte_clear_cnt);
    printk("ptep_get_and_clear_cnt [%llu]\n\n",ptep_get_and_clear_cnt);
    printk("ptep_get_and_clear_full_cnt [%llu]\n",ptep_get_and_clear_full_cnt);

	printk("all_set_pmd_cnt [%llu]\n",all_set_pmd_cnt);
	printk("set_pmd_at_cnt [%llu]\n",set_pmd_at_cnt);
	printk("native_set_pmd_cnt [%llu]\n",native_set_pmd_cnt);

	printk("all_pmd_clear_cnt [%llu]\n",all_pmd_clear_cnt);
	printk("native_pmd_clear_cnt [%llu]\n",native_pmd_clear_cnt);
	printk("pmdp_huge_get_and_clear_cnt [%llu]\n",pmdp_huge_get_and_clear_cnt);


    printk("dump_set_pte_cnt [%llu]\n",dump_set_pte_cnt);
    printk("dump_set_other_cnt [%llu]\n",dump_set_other_cnt);
    page_table_print_end();

    all_set_pte_cnt = 0;
    set_pte_at_cnt =  0;
	native_set_pte_cnt =  0;

    all_pte_clear_cnt = 0;
    native_pte_clear_cnt = 0;
    ptep_get_and_clear_cnt = 0;
	ptep_get_and_clear_full_cnt =  0;

	all_set_pmd_cnt =  0;
	set_pmd_at_cnt =  0;
	native_set_pmd_cnt =  0;

	all_pmd_clear_cnt =  0;
	native_pmd_clear_cnt =  0;
	pmdp_huge_get_and_clear_cnt =  0;


    dump_set_pte_cnt = 0;
    dump_set_other_cnt = 0;

    return 0;
}

/***********************************************************************************************
 * init the printk_for_trace function pointer to an empty fuction
 * ********************************************************************************************/
void printk_nothing(void *ptr, size_t size)
{
}

void  (*printk_for_trace)(void *ptr, size_t size) = &printk_nothing;

//printk_for_trace = &printk_nothing;


inline void page_table_trace_printk(void * ptr,size_t size)
{
    (*printk_for_trace)(ptr,size);
}

EXPORT_SYMBOL(page_table_trace_printk);
//////////////////////////////////////////
////
//// Dump Page Table
////
///////////////////////////////////////////

int dump_pte_range(int pid, pmd_t* pmd, unsigned long addr, unsigned long end)
{
        pte_t *pte;
        int   pfn;
again:
	/* Modified by Zhang Jiutian, 28/3/2014 */
	//pte = pte_offset_map_nested(pmd, addr);
	pte = pte_offset_map(pmd, addr);

	do {

            if (pte_none(*pte)) 
	        continue;
         
            pfn = (pte->pte) >> PAGE_SHIFT;
            if(pte_present(*pte) && pfn_valid(pfn)){
#ifdef PAGE_TABLE_USE_SPINLOCK                         
                        spin_lock_irqsave(&e1k_dma_lock,page_table_flags);
#endif
                page_table_val[0] = set_page_table_magic;                //1. fill magic char
//		page_table_val[0] = set_pt_addr_magic;                //1. fill magic char
                pid_trace = (unsigned int*)(&(page_table_val[1]));       //2. fill pid
                pte_trace = (unsigned long long*)(&(page_table_val[5])); //3. fill pte
            
                *pid_trace = pid; //(mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
                *pte_trace = (unsigned long long)(((addr >> PAGE_SHIFT) << 24)
                           | pte_pfn(*pte) /*(pte->pte >> PAGE_SHIFT)*/);
 
            dump_set_pte_cnt ++;
                page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);
#ifdef PAGE_TABLE_USE_SPINLOCK                         
                        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags);
#endif		
		if(page_table_kernel_printk)	
		{
			if( num_pte_kernel_printk < max_num_pte_kernel_printk ){
				printk("<1>## <%d>th type:0x%x,pid:%d,vpn:%llu,ppn:%lu\n",num_pte_kernel_printk,set_page_table_magic,pid,(addr >> PAGE_SHIFT),(pte->pte >> PAGE_SHIFT));
				num_pte_kernel_printk++;
			}
		}
		
            }
	} while (pte++, addr += PAGE_SIZE, addr != end);

	/* Modified by Zhang Jiutian, 28/3/2014 */
	//pte_unmap_nested(pte - 1); /*NOP*/
	pte_unmap(pte - 1); /*NOP*/
	if (addr != end)
		goto again;

	return 0;

}

int dump_pmd_range(int pid, pud_t* pud, unsigned long addr, unsigned long end)
{
	pmd_t *pmd;
	int pfn;
	unsigned long next;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd))
			continue;

#ifdef   DUMP_PT_ADDR_TRACE
		/////////////////////////////////////////////////////////////////////////////////////////////////
		//////////////////////////////////////////////////////////////////
		if(pt_addr_trace)
		{
#ifdef PAGE_TABLE_USE_SPINLOCK                         
                spin_lock_irqsave(&e1k_dma_lock,page_table_flags);
#endif
				pfn = (pmd->pmd) >> PAGE_SHIFT;
				if (pmd_large(*pmd) && pmd_present(*pmd) && pfn_valid(pfn)) {
					page_table_val[0] = set_huge_page_table_magic;                //1. fill magic char
                	pid_trace = (unsigned int*)(&(page_table_val[1]));       //2. fill pid
                	pmd_trace = (unsigned long long*)(&(page_table_val[5])); //3. fill pmd

                	*pid_trace = pid; //(mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
                	*pmd_trace = (unsigned long long)(((addr >> PMD_SHIFT) << 24) | pmd_pfn(*pmd));

                	page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);

					// zcg:
					// this pmd point to a large page,
					// we do not enter dump_pte_range for this pmd,
					// but before we continue while loop,
					// we need to unlock the spin_lock.
#ifdef PAGE_TABLE_USE_SPINLOCK                         
                    spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags);
#endif
					continue;   
				}

				dump_set_other_cnt ++;

#ifdef PRINTK_PT_ADDR_DEBUG

//                        printk("<1>dump pmd_pfn:virt=0x%lx,phys=0x%lx--%lx\n",(unsigned long)pmd,(pmd_val(*pmd) >> PAGE_SHIFT),__pa(pmd));
#endif			

#ifdef PAGE_TABLE_USE_SPINLOCK                         
                        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags);
#endif
		}
		/////////////////////////////////////////////////////////////////
		/////////////////////////////////////////////////////////////////////////////////////////////////		
#endif

		if (dump_pte_range(pid, pmd, addr, next))
			return -ENOMEM;
	} while (pmd++, addr = next, addr != end);
	return 0;
}

int dump_pud_range(int pid, p4d_t* p4d, unsigned long addr, unsigned long end)
{
    pud_t *pud;
    unsigned long next;

    pud = pud_offset(p4d, addr);
    do {
	next = pud_addr_end(addr, end);
	if (pud_none(*pud))
	    continue;

#ifdef   DUMP_PT_ADDR_TRACE
		/////////////////////////////////////////////////////////////////////////////////////////////////
                //////////////////////////////////////////////////////////////////
		if(pt_addr_trace)
		{
#ifdef PAGE_TABLE_USE_SPINLOCK                         
                        spin_lock_irqsave(&e1k_dma_lock,page_table_flags);
#endif
                	page_table_val[0] = set_pt_addr_magic;                //1. fill magic char
                	pid_trace = (unsigned int*)(&(page_table_val[1]));       //2. fill pid
                	pte_trace = (unsigned long long*)(&(page_table_val[5])); //3. fill pte

                	*pid_trace = pid; //(mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
                	*pte_trace = (unsigned long long)(((addr >> PAGE_SHIFT) << 24)
                        	   | (pud_val(*pud) >> PAGE_SHIFT));

//                	page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);
//			dump_set_other_cnt ++;

#ifdef PRINTK_PT_ADDR_DEBUG
                        //printk("<1>dump pud_pfn:0x%lu\n",(pud_val(*pud) >> PAGE_SHIFT));
//			printk("<1>dump pud_pfn:virt=0x%lx,phys=0x%lx--%lx\n",(unsigned long)pud,(pud_val(*pud) >> PAGE_SHIFT),__pa(pud));
#endif

#ifdef PAGE_TABLE_USE_SPINLOCK                         
                        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags);
#endif
		}
                /////////////////////////////////////////////////////////////////
                /////////////////////////////////////////////////////////////////////////////////////////////////   
#endif

        if (dump_pmd_range(pid, pud, addr, next))
  	    return -ENOMEM;
    } while (pud++, addr = next, addr != end);

    return 0;
}    

// lhf add
int dump_p4d_range(int pid, pgd_t* pgd, unsigned long addr, unsigned long end)
{
    p4d_t *p4d;
    unsigned long next;

    p4d = p4d_offset(pgd, addr);
    do {
    next = p4d_addr_end(addr, end);
    if (p4d_none(*p4d))
        continue;

#ifdef   DUMP_PT_ADDR_TRACE
        /////////////////////////////////////////////////////////////////////////////////////////////////
                //////////////////////////////////////////////////////////////////
        if(pt_addr_trace)
        {
#ifdef PAGE_TABLE_USE_SPINLOCK                         
                        spin_lock_irqsave(&e1k_dma_lock,page_table_flags);
#endif
                    page_table_val[0] = set_pt_addr_magic;                //1. fill magic char
                    pid_trace = (unsigned int*)(&(page_table_val[1]));       //2. fill pid
                    pte_trace = (unsigned long long*)(&(page_table_val[5])); //3. fill pte

                    *pid_trace = pid; //(mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
                    *pte_trace = (unsigned long long)(((addr >> PAGE_SHIFT) << 24)
                               | (p4d_val(*p4d) >> PAGE_SHIFT));

  //                  page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);
//		    dump_set_other_cnt ++;

#ifdef PRINTK_PT_ADDR_DEBUG
                        //printk("<1>dump p4d_pfn:0x%lu\n",(p4d_val(*p4d) >> PAGE_SHIFT));
//            printk("<1>dump p4d_pfn:virt=0x%lx,phys=0x%lx--%lx\n",(unsigned long)p4d,(p4d_val(*p4d) >> PAGE_SHIFT),__pa(p4d));
#endif

#ifdef PAGE_TABLE_USE_SPINLOCK                         
                        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags);
#endif
        }
                /////////////////////////////////////////////////////////////////
                /////////////////////////////////////////////////////////////////////////////////////////////////   
#endif

        if (dump_pud_range(pid, p4d, addr, next))
        return -ENOMEM;
    } while (p4d++, addr = next, addr != end);

    return 0;
}    


int dump_vm_area(int pid, struct mm_struct* mm, struct vm_area_struct* vma)
{
    
    pgd_t *pgd;
    unsigned long next;
    unsigned long addr = vma->vm_start;
    unsigned long end = vma->vm_end;
    
    pgd = pgd_offset(mm, addr);
    do {
	next = pgd_addr_end(addr, end);
	if (pgd_none(*pgd))
		continue;

#ifdef   DUMP_PT_ADDR_TRACE
		/////////////////////////////////////////////////////////////////////////////////////////////////
                //////////////////////////////////////////////////////////////////
                if(pt_addr_trace)
		{
#ifdef PAGE_TABLE_USE_SPINLOCK                         
      		      	spin_lock_irqsave(&e1k_dma_lock,page_table_flags);
#endif
			page_table_val[0] = set_pt_addr_magic;                //1. fill magic char
                	pid_trace = (unsigned int*)(&(page_table_val[1]));       //2. fill pid
                	pte_trace = (unsigned long long*)(&(page_table_val[5])); //3. fill pte

                	*pid_trace = pid; //(mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
                	*pte_trace = (unsigned long long)(((addr >> PAGE_SHIFT) << 24)
                        	   | (pgd_val(*pgd) >> PAGE_SHIFT));

//                	page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);

#ifdef PRINTK_PT_ADDR_DEBUG
                        //printk("<1>dump pgd_pfn:0x%lu\n",(pgd_val(*pgd) >> PAGE_SHIFT));
//			printk("<1>dump pgd_pfn:virt=0x%lx,phys=0x%lx--%lx\n",(unsigned long)pgd,(pgd_val(*pgd) >> PAGE_SHIFT),__pa(pgd));
#endif

			
#ifdef PAGE_TABLE_USE_SPINLOCK                         
            		spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags);
#endif
		}
                /////////////////////////////////////////////////////////////////
                ///////////////////////////////////////////////////////////////////////////////////////////////// 
#endif


	if (dump_p4d_range(pid, pgd, addr, next))
		return -ENOMEM;
    } while (pgd++, addr = next, addr != end);

    return 0;
}



int dump_page_table_trace(void)
{
    /*
 *      * for dump pagetable
 *           */
    struct task_struct *p;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    int pid;
    int dump_count = 0;
   
	if(page_table_kernel_printk)
        {
	        printk("<1>##############    Start Dump pagetable    ##################\n");
	}
 
    /* step.1 dump other processes pagetable */

    for(p = &init_task; (p = next_task(p)) != &init_task; ) {
        pid  = (p==&init_task) ? -1: p->pid;
        printk("<1>pid=%5d  cmd=%s\n", pid, p->comm);
        mm = p->mm;
        if(mm == NULL)
        {
	   //return 0;
		continue;
	}	

        spin_lock(&mm->page_table_lock);
        for(vma = mm->mmap; vma; vma = vma->vm_next)
            dump_vm_area(pid, mm, vma);
        spin_unlock(&mm->page_table_lock);
        
        dump_count ++;
    }//endof for(; p != &init_mm.mmlist; p = p->next)

	if(page_table_kernel_printk)
        {
                printk("<1>##############    End Dump pagetable    ##################\n");
		num_pte_kernel_printk = 0;
        }

    printk("<1>#### Dumped process number: %d\n", dump_count);
    return 0;

}

EXPORT_SYMBOL(printk_nothing);
EXPORT_SYMBOL(printk_for_trace);
EXPORT_SYMBOL(dump_page_table_trace);
EXPORT_SYMBOL(page_table_start_trace);
EXPORT_SYMBOL(page_table_stop_trace);
EXPORT_SYMBOL(page_table_stop_and_clear_trace);

EXPORT_SYMBOL(page_table_set_kernel_printk);
EXPORT_SYMBOL(page_table_reset_kernel_printk);

#ifdef   DUMP_PT_ADDR_TRACE
EXPORT_SYMBOL(pt_addr_start_trace);
EXPORT_SYMBOL(pt_addr_stop_trace);
#endif

void inc_all_set_pte_cnt(void)             {if(page_table_trace) all_set_pte_cnt++;}
void inc_set_pte_at_cnt(void)              {if(page_table_trace) set_pte_at_cnt++;}
void inc_native_set_pte_cnt(void)          {if(page_table_trace) native_set_pte_cnt++;}

void inc_all_pte_clear_cnt(void)           {if(page_table_trace) all_pte_clear_cnt++;}
void inc_native_pte_clear_cnt(void)        {if(page_table_trace) native_pte_clear_cnt++;}
void inc_ptep_get_and_clear_cnt(void)      {if(page_table_trace) ptep_get_and_clear_cnt++;}
void inc_ptep_get_and_clear_full_cnt(void) {if(page_table_trace) ptep_get_and_clear_full_cnt++;}

void inc_all_set_pmd_cnt(void)             {if(page_table_trace) all_set_pmd_cnt++;}
void inc_set_pmd_at_cnt(void)              {if(page_table_trace) set_pmd_at_cnt++;}
void inc_native_set_pmd_cnt(void)          {if(page_table_trace) native_set_pmd_cnt++;}

void inc_all_pmd_clear_cnt (void)          {if(page_table_trace) all_pmd_clear_cnt++;}
void inc_native_pmd_clear_cnt(void)        {if(page_table_trace) native_pmd_clear_cnt++;}
void inc_pmdp_huge_get_and_clear_cnt(void) {if(page_table_trace) pmdp_huge_get_and_clear_cnt++;}

#endif



void set_pte_at(struct mm_struct *mm, unsigned long addr,
				     pte_t *ptep , pte_t pte)
{
	inc_all_set_pte_cnt();
	inc_set_pte_at_cnt();

	page_table_check_pte_set(mm, addr, ptep, pte);
	//set_pte(ptep, pte); // we do not want this set_pte to be recorded, because we have record the set_pte_at
	WRITE_ONCE(*ptep, pte);

#if 1
#ifdef PAGE_TABLE_DEBUG__                       
    if(page_table_trace){     
#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_lock_irqsave(&e1k_dma_lock,page_table_flags); 
#endif

        page_table_val[0] = set_page_table_magic;
        pid_trace = (unsigned int*)(&(page_table_val[1]));
        pte_trace = (unsigned long long*)(&(page_table_val[5]));
            
        *pid_trace = (mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
        *pte_trace = (unsigned long long)(((addr >> PAGE_SHIFT) << 24) | pte_pfn(pte));

        page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);
		 
		if(page_table_kernel_printk) {
			if( /*num_pte_kernel_printk < max_num_pte_kernel_printk &&*/ /*pte.pte >= (2048ULL << 20) && pte.pte < (2560ULL<<20)*/ 1 ){
				printk("<1>## type:0x%x,pid:%d,vpn:0x%llx,pte_val:0x%lx,pte:0x%llx--0x%lx,0x%lx\n",
						set_page_table_magic,*pid_trace,(addr ),( pte_val(pte) ),*pte_trace,((addr >> PAGE_SHIFT) << 24),(pte.pte));
				num_pte_kernel_printk ++;
			}
        }

#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags); 
#endif                                          
    }
#endif //PAGE_TABLE_DEBUG__
#endif
}
EXPORT_SYMBOL(set_pte_at);

void native_set_pte(pte_t *ptep, pte_t pte)
{
	inc_all_set_pte_cnt();
	inc_native_set_pte_cnt();
	
	WRITE_ONCE(*ptep, pte);

#if 1
#ifdef PAGE_TABLE_DEBUG__                       
	if(page_table_trace){
#ifdef PAGE_TABLE_USE_SPINLOCK   
        spin_lock_irqsave(&e1k_dma_lock,page_table_flags);
#endif

        page_table_val[0] = set_page_table_magic;
        pid_trace = (unsigned int*)(&(page_table_val[1]));
        pte_trace = (unsigned long long*)(&(page_table_val[5]));

        *pid_trace = (current->mm == &init_mm)? (unsigned int)-1: (unsigned int)(current->pid);
        *pte_trace = (unsigned long long)(pte.pte);

        page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);

	    if(page_table_kernel_printk) {
	        if( /*num_pte_kernel_printk < max_num_pte_kernel_printk &&*/ /*pte.pte >= (2048ULL << 20) && pte.pte < (2560ULL<<20)*/ 1 ){
	            // printk("<1>## type:0x%x,pid:%d,vpn:0x%llx,pte_val:0x%lx,pte:0x%llx--0x%lx,0x%lx\n",set_page_table_magic,*pid_trace,(addr ),( pte_val(pte) ),*pte_trace,((addr >> PAGE_SHIFT) << 24),(pte.pte));
                            // num_pte_kernel_printk ++;
            }
        }

#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags);
#endif
     }
#endif //PAGE_TABLE_DEBUG__
#endif
}
EXPORT_SYMBOL(native_set_pte);

void native_pte_clear(struct mm_struct *mm, unsigned long addr,
				    pte_t *ptep)
{
	inc_all_pte_clear_cnt();
	inc_native_pte_clear_cnt();

#ifdef PAGE_TABLE_DEBUG__                       
    if(addr && page_table_trace){     
#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_lock_irqsave(&e1k_dma_lock,page_table_flags); 
#endif
            
        page_table_val[0] = free_page_table_magic;
        pid_trace = (unsigned int*)(&(page_table_val[1]));
        pte_trace = (unsigned long long*)(&(page_table_val[5]));
            
        *pid_trace = (mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
        *pte_trace = (unsigned long long)(((addr >> PAGE_SHIFT) << 24) | pte_pfn(*ptep));

        page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);
            
		if(page_table_kernel_printk) {
			if( num_pte_kernel_printk < max_num_pte_kernel_printk ) {
				printk("<1>## type:0x%x,pid:%d,vpn:%llu,ppn:%lu\n",
						free_page_table_magic,*pid_trace,(addr >> PAGE_SHIFT),(ptep->pte >> PAGE_SHIFT));
				num_pte_kernel_printk++;
			}
        }

#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags); 
#endif                                          
    }
#endif //PAGE_TABLE_DEBUG__

	native_set_pte(ptep, native_make_pte(0));
}

pte_t ptep_get_and_clear(struct mm_struct *mm, unsigned long addr,
				       pte_t *ptep)
{
    pte_t pte;
	
	inc_all_pte_clear_cnt();
    inc_ptep_get_and_clear_cnt();

#ifdef PAGE_TABLE_DEBUG__                       
    if(page_table_trace) {     
#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_lock_irqsave(&e1k_dma_lock,page_table_flags); 
#endif

        page_table_val[0] = free_page_table_get_clear;
        pid_trace = (unsigned int*)(&(page_table_val[1]));
        pte_trace = (unsigned long long*)(&(page_table_val[5]));
            
        *pid_trace = (mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
        *pte_trace = (unsigned long long)(((addr >> PAGE_SHIFT) << 24) | pte_pfn(*ptep));

        page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);
            
		if(page_table_kernel_printk) {
			if( num_pte_kernel_printk < max_num_pte_kernel_printk ) {
				printk("<1>## type:0x%x,pid:%d,vpn:%llu,ppn:%lu\n",
						free_page_table_get_clear,*pid_trace,(addr >> PAGE_SHIFT),(ptep->pte >> PAGE_SHIFT));
				num_pte_kernel_printk ++;
			}
        }

#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags); 
#endif                                          
    }
#endif //PAGE_TABLE_DEBUG__

    pte = native_ptep_get_and_clear(ptep);
	page_table_check_pte_clear(mm, addr, pte);
	return pte;
}

pte_t ptep_get_and_clear_full(struct mm_struct *mm,
			    unsigned long addr, pte_t *ptep,
			    int full)
{
	pte_t pte;

	inc_all_pte_clear_cnt();
    inc_ptep_get_and_clear_full_cnt();

	if (full) {
		/*
 		 * Full address destruction in progress; paravirt does not
 		 * care about updates and native needs no locking
 		 *  	
 		 */
           
        // HMTT NOTATION: 
		// In this path, native_pte_clear(NULL, 0, ptep) is called.
        // We have set a filter of (addr != 0) in native_pte_clear()
        // in order to bypass this path.

#ifdef PAGE_TABLE_DEBUG__                       
        if(page_table_trace){     
#ifdef PAGE_TABLE_USE_SPINLOCK                         
            spin_lock_irqsave(&e1k_dma_lock,page_table_flags); 
#endif
            page_table_val[0] = free_page_table_get_clear_full;
            pid_trace = (unsigned int*)(&(page_table_val[1]));
            pte_trace = (unsigned long long*)(&(page_table_val[5]));
            
            *pid_trace = (mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
            *pte_trace = (unsigned long long)(((addr >> PAGE_SHIFT) << 24) | pte_pfn(*ptep));

            page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);
            if(page_table_kernel_printk) {
				if( num_pte_kernel_printk < max_num_pte_kernel_printk ) {
					printk("<1>## type:0x%x,pid:%d,vpn:%llu,ppn:%lu\n",
							free_page_table_get_clear_full,*pid_trace,(addr >> PAGE_SHIFT),(ptep->pte >> PAGE_SHIFT));
					num_pte_kernel_printk ++;
				}
            }
#ifdef PAGE_TABLE_USE_SPINLOCK                         
            spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags); 
#endif                                          
        }
#endif //PAGE_TABLE_DEBUG__
                
		pte = native_local_ptep_get_and_clear(ptep);
		page_table_check_pte_clear(mm, addr, pte);
	} else {
		pte = ptep_get_and_clear(mm, addr, ptep);
	}
	return pte;
}

void set_pmd_at(struct mm_struct *mm, unsigned long addr,
			      pmd_t *pmdp, pmd_t pmd)
{
	inc_all_set_pmd_cnt();
	inc_set_pmd_at_cnt();

	page_table_check_pmd_set(mm, addr, pmdp, pmd);
	// set_pmd(pmdp, pmd); // we do not want this set_pmd to be recorded, because we have record the set_pmd_at
	WRITE_ONCE(*pmdp, pmd);

#if 1
#ifdef PAGE_TABLE_DEBUG__                       
    if(page_table_trace){     
#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_lock_irqsave(&e1k_dma_lock,page_table_flags); 
#endif
            
        page_table_val[0] = set_huge_page_table_magic;
        pid_trace = (unsigned int*)(&(page_table_val[1]));
        pmd_trace = (unsigned long long*)(&(page_table_val[5]));
            
        *pid_trace = (mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
        *pmd_trace = (unsigned long long)(((addr >> PMD_SHIFT) << 24) | pmd_pfn(pmd));

		// check PSE bit of pmd entry to decide 
		// whether to call page_table_trace_printk() or not
		if (pmd_large(pmd)) {
			page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);
		 
			if(page_table_kernel_printk) {
				if( /*num_pte_kernel_printk < max_num_pte_kernel_printk &&*/ /*pte.pte >= (2048ULL << 20) && pte.pte < (2560ULL<<20)*/ 1 ){
					printk("<1>## type:0x%x,pid:%d,vpn:0x%llx,pmd_val:0x%lx,pmd:0x%llx--0x%lx,0x%lx\n",
							set_huge_page_table_magic,*pid_trace,(addr ),( pmd_val(pmd) ),*pmd_trace,((addr >> PMD_SHIFT) << 24),(pmd.pmd));
					num_pte_kernel_printk ++;
				}
        	}
		}       

#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags); 
#endif                                          
    }
#endif //PAGE_TABLE_DEBUG__
#endif
}
EXPORT_SYMBOL(set_pmd_at);

void native_set_pmd(pmd_t *pmdp, pmd_t pmd)
{
	inc_all_set_pmd_cnt();
	inc_native_set_pmd_cnt();

#ifdef PAGE_TABLE_DEBUG__                       
	if(page_table_trace){
#ifdef PAGE_TABLE_USE_SPINLOCK   
        spin_lock_irqsave(&e1k_dma_lock,page_table_flags);
#endif

        page_table_val[0] = set_huge_page_table_magic;
        pid_trace = (unsigned int*)(&(page_table_val[1]));
        pmd_trace = (unsigned long long*)(&(page_table_val[5]));

        *pid_trace = (current->mm == &init_mm)? (unsigned int)-1: (unsigned int)(current->pid);
        *pmd_trace = (unsigned long long)(pmd.pmd);

		// check PSE bit of pmd entry to decide 
		// whether to call page_table_trace_printk() or not
		if (pmd_large(pmd)) {
			page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);

	    	if(page_table_kernel_printk) {
	        	if( /*num_pte_kernel_printk < max_num_pte_kernel_printk &&*/ /*pte.pte >= (2048ULL << 20) && pte.pte < (2560ULL<<20)*/ 1 ){
	            	// printk("<1>## type:0x%x,pid:%d,vpn:0x%llx,pte_val:0x%lx,pte:0x%llx--0x%lx,0x%lx\n",set_huge_page_table_magic,*pid_trace,(addr ),( pte_val(pte) ),*pte_trace,((addr >> PAGE_SHIFT) << 24),(pte.pte));
                            	// num_pte_kernel_printk ++;
            	}
        	}
		}  

#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags);
#endif
     }
#endif //PAGE_TABLE_DEBUG__

	WRITE_ONCE(*pmdp, pmd);
}
EXPORT_SYMBOL(native_set_pmd);

void native_pmd_clear(pmd_t *pmd)
{
	inc_all_pmd_clear_cnt();
	inc_native_pmd_clear_cnt();

#ifdef PAGE_TABLE_DEBUG__                       
    if(page_table_trace){     
#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_lock_irqsave(&e1k_dma_lock,page_table_flags); 
#endif
            
        page_table_val[0] = free_huge_page_table_magic;
        pid_trace = (unsigned int*)(&(page_table_val[1]));
        pmd_trace = (unsigned long long*)(&(page_table_val[5]));
            
        *pid_trace = (current->mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
        *pmd_trace = pmd->pmd;   // we don't know vpn

		// check PSE bit of pmd entry to decide 
		// whether to call page_table_trace_printk() or not
		if (pmd_large(*pmd)) {
			page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);
            
			if(page_table_kernel_printk) {
				if( num_pte_kernel_printk < max_num_pte_kernel_printk ) {
					printk("<1>## type:0x%x,pid:%d,vpn:%llu,ppn:%lu\n",
							free_huge_page_table_magic,*pid_trace, 0 /*we don't know vpn*/,(pmd->pmd >> PMD_SHIFT));
					num_pte_kernel_printk++;
				}
        	}
		}

#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags); 
#endif                                          
    }
#endif //PAGE_TABLE_DEBUG__

	native_set_pmd(pmd, native_make_pmd(0));
}

pmd_t pmdp_huge_get_and_clear(struct mm_struct *mm, unsigned long addr,
				       pmd_t *pmdp)
{
	inc_all_pmd_clear_cnt();
	inc_pmdp_huge_get_and_clear_cnt();

#ifdef PAGE_TABLE_DEBUG__                       
    if(page_table_trace) {     
#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_lock_irqsave(&e1k_dma_lock,page_table_flags); 
#endif

        page_table_val[0] = free_huge_page_table_get_clear;
        pid_trace = (unsigned int*)(&(page_table_val[1]));
        pmd_trace = (unsigned long long*)(&(page_table_val[5]));
            
        *pid_trace = (mm == &init_mm)? (unsigned int)-1: (unsigned int)current->pid;
        *pmd_trace = (unsigned long long)(((addr >> PMD_SHIFT) << 24) | pmd_pfn(*pmdp));

		// check PSE bit of pmd entry to decide 
		// whether to call page_table_trace_printk() or not
		if (pmd_large(*pmdp)) {
			page_table_trace_printk(page_table_val, PAGE_TABLE_TRACE_SIZE);
            
			if(page_table_kernel_printk) {
				if( num_pte_kernel_printk < max_num_pte_kernel_printk ) {
					printk("<1>## type:0x%x,pid:%d,vpn:%llu,ppn:%lu\n",
							free_huge_page_table_get_clear,*pid_trace,(addr >> PMD_SHIFT),(pmdp->pmd >> PMD_SHIFT));
					num_pte_kernel_printk ++;
				}
        	}
		}

#ifdef PAGE_TABLE_USE_SPINLOCK                         
        spin_unlock_irqrestore(&e1k_dma_lock,page_table_flags); 
#endif                                          
    }
#endif //PAGE_TABLE_DEBUG__

	pmd_t pmd = native_pmdp_get_and_clear(pmdp);
	page_table_check_pmd_clear(mm, addr, pmd);
	return pmd;
}

void printk_io_nothing(void *ptr, size_t size)
{
}

void  (*printk_io_trace)(void *ptr, size_t size) = &printk_io_nothing;
int flag_start_io_trace = 0;
long long ALL_num_IO = 0;
long long num_mpage = 0;
long long num_iomap = 0;
long long num_buf = 0;
long long num_aop = 0;


void set_flag_start_io_trace(int flag){
	flag_start_io_trace = flag;	
}

inline int get_flag_start_io_trace(int tmp){
	return flag_start_io_trace;
}

inline void io_trace_printk(void *ptr, size_t size){
	(*printk_io_trace)(ptr,size);
}
EXPORT_SYMBOL(printk_io_nothing);
EXPORT_SYMBOL(printk_io_trace);
EXPORT_SYMBOL(io_trace_printk);
EXPORT_SYMBOL(flag_start_io_trace);
EXPORT_SYMBOL(set_flag_start_io_trace);
EXPORT_SYMBOL(get_flag_start_io_trace);
EXPORT_SYMBOL(ALL_num_IO);
EXPORT_SYMBOL(num_mpage);
EXPORT_SYMBOL(num_iomap);
EXPORT_SYMBOL(num_buf);
EXPORT_SYMBOL(num_aop);

/************************************************************************************
 *  Printk PageTable Infomation -- End
 ***********************************************************************************/