#include <common/util.h>
#include <common/vars.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/errno.h>
#include <lib/printk.h>
#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <arch/mmu.h>

#include <arch/mm/page_table.h>

extern void set_ttbr0_el1(paddr_t);

void set_page_table(paddr_t pgtbl)
{
        set_ttbr0_el1(pgtbl);
}

extern u64 boot_ttbr0_l0[PTP_ENTRIES];
extern u64 boot_ttbr0_l1[PTP_ENTRIES];
extern u64 boot_ttbr0_l2[PTP_ENTRIES];

#define USER_PTE 0
/* number of page table level in Armv8 */
#define NUMBER_OF_LEVEL 4

/*
 * the 3rd arg means the kind of PTE.
 */
static int set_pte_flags(pte_t *entry, vmr_prop_t flags, int kind)
{
        // Only consider USER PTE now.
        BUG_ON(kind != USER_PTE);

        /*
         * Current access permission (AP) setting:
         * Mapped pages are always readable (No considering
         * XOM(eXecute-Only-Memory)). EL1 can directly access EL0 (No
         * restriction like SMAP(Supervisor Mode Access Prevention) as ChCore is
         * a microkernel).
         */
        if (flags & VMR_WRITE)
                entry->l3_page.AP = AARCH64_MMU_ATTR_PAGE_AP_HIGH_RW_EL0_RW;
        else
                entry->l3_page.AP = AARCH64_MMU_ATTR_PAGE_AP_HIGH_RO_EL0_RO;

        if (flags & VMR_EXEC)
                entry->l3_page.UXN = AARCH64_MMU_ATTR_PAGE_UX;
        else
                entry->l3_page.UXN = AARCH64_MMU_ATTR_PAGE_UXN;

        // EL1 cannot directly execute EL0 accessiable region.
        entry->l3_page.PXN = AARCH64_MMU_ATTR_PAGE_PXN;
        // Set AF (access flag) in advance.
        entry->l3_page.AF = AARCH64_MMU_ATTR_PAGE_AF_ACCESSED;
        // Mark the mapping as not global
        entry->l3_page.nG = 1;
        // Mark the mappint as inner sharable
        entry->l3_page.SH = INNER_SHAREABLE;
        // Set the memory type
        if (flags & VMR_DEVICE) {
                entry->l3_page.attr_index = DEVICE_MEMORY;
                entry->l3_page.SH = 0;
        } else if (flags & VMR_NOCACHE) {
                entry->l3_page.attr_index = NORMAL_MEMORY_NOCACHE;
        } else {
                entry->l3_page.attr_index = NORMAL_MEMORY;
        }

        return 0;
}

#define GET_PADDR_IN_PTE(entry) \
        (((u64)entry->table.next_table_addr) << PAGE_SHIFT)
#define GET_NEXT_PTP(entry) phys_to_virt(GET_PADDR_IN_PTE(entry))

#define NORMAL_PTP (0)
#define BLOCK_PTP  (1)

/*
 * Find next page table page for the "va".
 *
 * cur_ptp: current page table page
 * level:   current ptp level
 *
 * next_ptp: returns "next_ptp"
 * pte     : returns "pte" (points to next_ptp) in "cur_ptp"
 *
 * alloc: if true, allocate a ptp when missing
 *
 */
static int get_next_ptp(ptp_t *cur_ptp, u32 level, vaddr_t va, ptp_t **next_ptp,
                        pte_t **pte, bool alloc)
{
        u32 index = 0;
        pte_t *entry;

        if (cur_ptp == NULL)
                return -ENOMAPPING;

        switch (level) {
        case 0:
                index = GET_L0_INDEX(va);
                break;
        case 1:
                index = GET_L1_INDEX(va);
                break;
        case 2:
                index = GET_L2_INDEX(va);
                break;
        case 3:
                /* shouldn't be here */
                index = GET_L3_INDEX(va);
                break;
        default:
                BUG_ON(1);
        }

        entry = &(cur_ptp->ent[index]); // we use a pointer because we may need
                                        // to modify entry
        if (IS_PTE_INVALID(entry->pte)) {
                if (alloc == false) {
                        return -ENOMAPPING;
                } else {
                        /* alloc a new page table page */
                        ptp_t *new_ptp;
                        paddr_t new_ptp_paddr;
                        pte_t new_pte_val;

                        /* alloc a single physical page as a new page table page
                         */
                        new_ptp = get_pages(0);
                        BUG_ON(new_ptp == NULL);
                        memset((void *)new_ptp, 0, PAGE_SIZE);
                        new_ptp_paddr = virt_to_phys((vaddr_t)new_ptp);

                        new_pte_val.pte = 0; // clear pte to zero
                        new_pte_val.table.is_valid = 1;
                        new_pte_val.table.is_table = 1;
                        new_pte_val.table.next_table_addr = new_ptp_paddr
                                                            >> PAGE_SHIFT;

                        /* same effect as: cur_ptp->ent[index] = new_pte_val; */
                        entry->pte = new_pte_val.pte;
                }
        }

        *next_ptp = (ptp_t *)GET_NEXT_PTP(entry);
        *pte = entry;
        if (IS_PTE_TABLE(entry->pte))
                return NORMAL_PTP;
        else
                return BLOCK_PTP;
}

void free_page_table(void *pgtbl)
{
        ptp_t *l0_ptp, *l1_ptp, *l2_ptp, *l3_ptp;
        pte_t *l0_pte, *l1_pte, *l2_pte;
        int i, j, k;

        if (pgtbl == NULL) {
                kwarn("%s: input arg is NULL.\n", __func__);
                return;
        }

        /* L0 page table */
        l0_ptp = (ptp_t *)pgtbl;

        /* Iterate each entry in the l0 page table*/
        for (i = 0; i < PTP_ENTRIES; ++i) {
                l0_pte = &l0_ptp->ent[i];
                if (IS_PTE_INVALID(l0_pte->pte) || !IS_PTE_TABLE(l0_pte->pte))
                        continue;
                l1_ptp = (ptp_t *)GET_NEXT_PTP(l0_pte);

                /* Iterate each entry in the l1 page table*/
                for (j = 0; j < PTP_ENTRIES; ++j) {
                        l1_pte = &l1_ptp->ent[j];
                        if (IS_PTE_INVALID(l1_pte->pte)
                            || !IS_PTE_TABLE(l1_pte->pte))
                                continue;
                        l2_ptp = (ptp_t *)GET_NEXT_PTP(l1_pte);

                        /* Interate each entry in the l2 page table*/
                        for (k = 0; k < PTP_ENTRIES; ++k) {
                                l2_pte = &l2_ptp->ent[k];
                                if (IS_PTE_INVALID(l2_pte->pte)
                                    || !IS_PTE_TABLE(l2_pte->pte))
                                        continue;
                                l3_ptp = (ptp_t *)GET_NEXT_PTP(l2_pte);
                                /* Free the l3 page table page */
                                free_pages(l3_ptp);
                        }

                        /* Free the l2 page table page */
                        free_pages(l2_ptp);
                }

                /* Free the l1 page table page */
                free_pages(l1_ptp);
        }

        free_pages(l0_ptp);
}

/*
 * Translate a va to pa, and get its pte for the flags
 */
int query_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t *pa, pte_t **entry)
{
        /* LAB 2 TODO 3 BEGIN */
        /*
         * Hint: Walk through each level of page table using `get_next_ptp`,
         * return the pa and pte until a L1/L2 block or page, return
         * `-ENOMAPPING` if the va is not mapped.
         */

        if (pgtbl == NULL) {
                kwarn("%s: input arg is NULL.\n", __func__);
                return -1;
        }
        ptp_t *cur_ptp = pgtbl, *next_ptp;
        pte_t *pte_p;
        for (int level = 0; level < NUMBER_OF_LEVEL - 1; level++) {
                int result = get_next_ptp(
                        cur_ptp, level, va, &next_ptp, &pte_p, false);
                if (result == -ENOMAPPING) {
                        return -ENOMAPPING;
                } else if (result == NORMAL_PTP) {
                        cur_ptp = next_ptp;
                } else if (result == BLOCK_PTP) {
                        *entry = pte_p;
                        switch (level) {
                        case 1:
                                *pa = (u64)(pte_p->l1_block.pfn)
                                              << L1_INDEX_SHIFT
                                      | GET_VA_OFFSET_L1(va);
                                break;
                        case 2:
                                *pa = (u64)(pte_p->l2_block.pfn)
                                              << L2_INDEX_SHIFT
                                      | GET_VA_OFFSET_L2(va);
                                break;
                        }
                        return 0;
                } else {
                        BUG("Undefined result of get_next_ptp");
                }
        }
        pte_p = &(cur_ptp->ent[GET_L3_INDEX(va)]);
        if (IS_PTE_INVALID(pte_p->pte))
                return -ENOMAPPING;
        *pa = (u64)(pte_p->l3_page.pfn) << PAGE_SHIFT | GET_VA_OFFSET_L3(va);
        *entry = pte_p;
        return 0;
        /* LAB 2 TODO 3 END */
}

int map_range_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t pa, size_t len,
                       vmr_prop_t flags)
{
        /* LAB 2 TODO 3 BEGIN */
        /*
         * Hint: Walk through each level of page table using `get_next_ptp`,
         * create new page table page if necessary, fill in the final level
         * pte with the help of `set_pte_flags`. Iterate until all pages are
         * mapped.
         */
        if (pgtbl == NULL) {
                kwarn("%s: input arg is NULL.\n", __func__);
                return -1;
        }
        while ((long)len > 0) {
                ptp_t *cur_ptp = pgtbl, *next_ptp;
                pte_t *pte_p;
                for (int level = 0; level < NUMBER_OF_LEVEL - 1; level++) {
                        int result = get_next_ptp(
                                cur_ptp, level, va, &next_ptp, &pte_p, true);
                        BUG_ON(result != NORMAL_PTP);
                        cur_ptp = next_ptp;
                }
                pte_p = &(next_ptp->ent[GET_L3_INDEX(va)]);
                pte_p->pte = 0;
                pte_p->l3_page.is_valid = 1;
                pte_p->l3_page.is_page = 1;
                pte_p->l3_page.pfn = pa >> PAGE_SHIFT;
                set_pte_flags(pte_p, flags, USER_PTE);
                len -= PAGE_SIZE;
                va += PAGE_SIZE;
                pa += PAGE_SIZE;
        }
        return 0;
        /* LAB 2 TODO 3 END */
}

int unmap_range_in_pgtbl(void *pgtbl, vaddr_t va, size_t len)
{
        /* LAB 2 TODO 3 BEGIN */
        /*
         * Hint: Walk through each level of page table using `get_next_ptp`,
         * mark the final level pte as invalid. Iterate until all pages are
         * unmapped.
         */
        if (pgtbl == NULL) {
                kwarn("%s: input arg is NULL.\n", __func__);
                return -1;
        }
        while ((long)len > 0) {
                ptp_t *cur_ptp = pgtbl, *next_ptp;
                pte_t *pte_p;
                for (int level = 0; level < NUMBER_OF_LEVEL - 1; level++) {
                        int result = get_next_ptp(
                                cur_ptp, level, va, &next_ptp, &pte_p, true);
                        BUG_ON(result != NORMAL_PTP);
                        cur_ptp = next_ptp;
                }
                pte_p = &(next_ptp->ent[GET_L3_INDEX(va)]);
                pte_p->l3_page.is_valid = 0;
                len -= PAGE_SIZE;
                va += PAGE_SIZE;
        }
        return 0;
        /* LAB 2 TODO 3 END */
}

int map_range_in_pgtbl_huge(void *pgtbl, vaddr_t va, paddr_t pa, size_t len,
                            vmr_prop_t flags)
{
        /* LAB 2 TODO 4 BEGIN */
        if (pgtbl == NULL) {
                kwarn("%s: input arg is NULL.\n", __func__);
                return -1;
        }
        while (len > PAGE_SIZE_1G) {
                ptp_t *cur_ptp = pgtbl, *next_ptp;
                pte_t *pte_p;
                int result =
                        get_next_ptp(cur_ptp, 0, va, &next_ptp, &pte_p, true);
                BUG_ON(result != NORMAL_PTP);
                pte_p = &(next_ptp->ent[GET_L1_INDEX(va)]);
                pte_p->l1_block.is_valid = 1;
                pte_p->l1_block.is_table = 0;
                pte_p->l1_block.pfn = pa >> L1_INDEX_SHIFT;
                set_pte_flags(pte_p, flags, USER_PTE);

                len -= PAGE_SIZE_1G;
                va += PAGE_SIZE_1G;
                pa += PAGE_SIZE_1G;
        }

        while (len > PAGE_SIZE_2M) {
                ptp_t *cur_ptp = pgtbl, *next_ptp;
                pte_t *pte_p;
                for (int level = 0; level < 2; level++) {
                        int result = get_next_ptp(
                                cur_ptp, level, va, &next_ptp, &pte_p, true);
                        BUG_ON(result != NORMAL_PTP);
                        cur_ptp = next_ptp;
                }
                pte_p = &(cur_ptp->ent[GET_L2_INDEX(va)]);
                pte_p->l2_block.is_valid = 1;
                pte_p->l2_block.is_table = 0;
                pte_p->l2_block.pfn = pa >> L2_INDEX_SHIFT;
                set_pte_flags(pte_p, flags, USER_PTE);

                len -= PAGE_SIZE_2M;
                va += PAGE_SIZE_2M;
                pa += PAGE_SIZE_2M;
        }

        return map_range_in_pgtbl(pgtbl, va, pa, len, flags);
        /* LAB 2 TODO 4 END */
}

int unmap_range_in_pgtbl_huge(void *pgtbl, vaddr_t va, size_t len)
{
        /* LAB 2 TODO 4 BEGIN */
        if (pgtbl == NULL) {
                kwarn("%s: input arg is NULL.\n", __func__);
                return -1;
        }
        while (len > PAGE_SIZE_1G) {
                ptp_t *cur_ptp = pgtbl, *next_ptp;
                pte_t *pte_p;
                int result =
                        get_next_ptp(cur_ptp, 0, va, &next_ptp, &pte_p, false);
                BUG_ON(result != NORMAL_PTP);
                pte_p = &(next_ptp->ent[GET_L1_INDEX(va)]);
                pte_p->l1_block.is_valid = 0;

                len -= PAGE_SIZE_1G;
                va += PAGE_SIZE_1G;
        }

        while (len > PAGE_SIZE_2M) {
                ptp_t *cur_ptp = pgtbl, *next_ptp;
                pte_t *pte_p;
                for (int level = 0; level < 2; level++) {
                        int result = get_next_ptp(
                                cur_ptp, level, va, &next_ptp, &pte_p, false);
                        BUG_ON(result != NORMAL_PTP);
                        cur_ptp = next_ptp;
                }
                pte_p = &(cur_ptp->ent[GET_L2_INDEX(va)]);
                pte_p->l2_block.is_valid = 0;

                len -= PAGE_SIZE_2M;
                va += PAGE_SIZE_2M;
        }

        return unmap_range_in_pgtbl(pgtbl, va, len);
        /* LAB 2 TODO 4 END */
}

#define HIGH_PHYSMEM_START   (0xffffff0000000000UL)
#define HIGH_PERIPHERAL_BASE (0xffffff003F000000UL)
#define HIGH_PHYSMEM_END     (0xffffff0040000000UL)

extern char init_end; // also .text start
extern char text_end;
extern char data_start;
extern char data_end;
extern char rodata_start;
extern char _edata;
extern char _bss_start;
extern char _bss_end;

void reset_pt()
{
        u64 init_end_v = (u64)&init_end;
        u64 text_end_v = (u64)&text_end;
        u64 data_start_v = (u64)&data_start;
        u64 data_end_v = (u64)&data_end;
        u64 rodata_start_v = (u64)&rodata_start;
        u64 _edata_v = (u64)&_edata;
        u64 _bss_start_v = (u64)&_bss_start;
        u64 _bss_end_v = (u64)&_bss_end;
        kwarn("init_end is %lx\ttext_end is %lx\tdata_start is %lx\t"
              "data_end is %lx\trodata_start is %lx\t_edata is %lx\t"
              "_bss_start is %lx\t_bss_end is %lx\n",
              init_end_v,
              text_end_v,
              data_start_v,
              data_end_v,
              rodata_start_v,
              _edata_v,
              _bss_start_v,
              _bss_end_v);
        u64 vaddr;
        vmr_prop_t flags;
        ptp_t *new_ttbr1_l0 = get_pages(0);

        /* .text if from init_end to text_end */
        flags = VMR_EXEC | VMR_READ;
        vaddr = HIGH_PHYSMEM_START + init_end_v;
        map_range_in_pgtbl_huge(new_ttbr1_l0,
                                vaddr,
                                virt_to_phys(vaddr),
                                text_end_v - init_end_v,
                                flags);
        /* .data section is from data_start to data_end */
        vaddr = HIGH_PHYSMEM_START + data_start_v;
        flags = VMR_READ | VMR_WRITE;
        map_range_in_pgtbl_huge(new_ttbr1_l0,
                                vaddr,
                                virt_to_phys(vaddr),
                                data_end_v - data_start_v,
                                flags);
        vaddr = HIGH_PHYSMEM_START + rodata_start_v;
        flags = VMR_READ;
        /* .rodata is from rodata start to _edata */
        map_range_in_pgtbl_huge(new_ttbr1_l0,
                                vaddr,
                                virt_to_phys(vaddr),
                                _edata_v - rodata_start_v,
                                flags);
        /* bss is from bss start to bss end */
        vaddr = HIGH_PHYSMEM_START + _bss_start_v;
        flags = VMR_READ | VMR_WRITE;
        map_range_in_pgtbl_huge(new_ttbr1_l0,
                                vaddr,
                                virt_to_phys(vaddr),
                                _bss_end_v - _bss_start_v,
                                flags);

        vaddr = HIGH_PERIPHERAL_BASE;
        flags = VMR_DEVICE;
        map_range_in_pgtbl_huge(new_ttbr1_l0,
                                vaddr,
                                virt_to_phys(vaddr),
                                HIGH_PHYSMEM_END - HIGH_PERIPHERAL_BASE,
                                flags);
        __asm__("msr ttbr1_el1,%0":"+r"(new_ttbr1_l0));
}

#ifdef CHCORE_KERNEL_TEST
#include <mm/buddy.h>
#include <lab.h>
void lab2_test_page_table(void)
{
        vmr_prop_t flags = VMR_READ | VMR_WRITE;
        {
                bool ok = true;
                void *pgtbl = get_pages(0);
                memset(pgtbl, 0, PAGE_SIZE);
                paddr_t pa;
                pte_t *pte;
                int ret;

                ret = map_range_in_pgtbl(
                        pgtbl, 0x1001000, 0x1000, PAGE_SIZE, flags);
                lab_assert(ret == 0);

                ret = query_in_pgtbl(pgtbl, 0x1001000, &pa, &pte);
                lab_assert(ret == 0 && pa == 0x1000);
                lab_assert(pte && pte->l3_page.is_valid && pte->l3_page.is_page
                           && pte->l3_page.SH == INNER_SHAREABLE);
                ret = query_in_pgtbl(pgtbl, 0x1001050, &pa, &pte);
                lab_assert(ret == 0 && pa == 0x1050);

                ret = unmap_range_in_pgtbl(pgtbl, 0x1001000, PAGE_SIZE);
                lab_assert(ret == 0);
                ret = query_in_pgtbl(pgtbl, 0x1001000, &pa, &pte);
                lab_assert(ret == -ENOMAPPING);

                free_page_table(pgtbl);
                lab_check(ok, "Map & unmap one page");
        }
        {
                bool ok = true;
                void *pgtbl = get_pages(0);
                memset(pgtbl, 0, PAGE_SIZE);
                paddr_t pa;
                pte_t *pte;
                int ret;
                size_t nr_pages = 10;
                size_t len = PAGE_SIZE * nr_pages;

                ret = map_range_in_pgtbl(pgtbl, 0x1001000, 0x1000, len, flags);
                lab_assert(ret == 0);
                ret = map_range_in_pgtbl(
                        pgtbl, 0x1001000 + len, 0x1000 + len, len, flags);
                lab_assert(ret == 0);

                for (int i = 0; i < nr_pages * 2; i++) {
                        ret = query_in_pgtbl(
                                pgtbl, 0x1001050 + i * PAGE_SIZE, &pa, &pte);
                        lab_assert(ret == 0 && pa == 0x1050 + i * PAGE_SIZE);
                        lab_assert(pte && pte->l3_page.is_valid
                                   && pte->l3_page.is_page);
                }

                ret = unmap_range_in_pgtbl(pgtbl, 0x1001000, len);
                lab_assert(ret == 0);
                ret = unmap_range_in_pgtbl(pgtbl, 0x1001000 + len, len);
                lab_assert(ret == 0);

                for (int i = 0; i < nr_pages * 2; i++) {
                        ret = query_in_pgtbl(
                                pgtbl, 0x1001050 + i * PAGE_SIZE, &pa, &pte);
                        lab_assert(ret == -ENOMAPPING);
                }

                free_page_table(pgtbl);
                lab_check(ok, "Map & unmap multiple pages");
        }
        {
                bool ok = true;
                void *pgtbl = get_pages(0);
                memset(pgtbl, 0, PAGE_SIZE);
                paddr_t pa;
                pte_t *pte;
                int ret;
                /* 1GB + 4MB + 40KB */
                size_t len = (1 << 30) + (4 << 20) + 10 * PAGE_SIZE;

                ret = map_range_in_pgtbl(
                        pgtbl, 0x100000000, 0x100000000, len, flags);
                lab_assert(ret == 0);
                ret = map_range_in_pgtbl(pgtbl,
                                         0x100000000 + len,
                                         0x100000000 + len,
                                         len,
                                         flags);
                lab_assert(ret == 0);

                for (vaddr_t va = 0x100000000; va < 0x100000000 + len * 2;
                     va += 5 * PAGE_SIZE + 0x100) {
                        ret = query_in_pgtbl(pgtbl, va, &pa, &pte);
                        lab_assert(ret == 0 && pa == va);
                }

                ret = unmap_range_in_pgtbl(pgtbl, 0x100000000, len);
                lab_assert(ret == 0);
                ret = unmap_range_in_pgtbl(pgtbl, 0x100000000 + len, len);
                lab_assert(ret == 0);

                for (vaddr_t va = 0x100000000; va < 0x100000000 + len;
                     va += 5 * PAGE_SIZE + 0x100) {
                        ret = query_in_pgtbl(pgtbl, va, &pa, &pte);
                        lab_assert(ret == -ENOMAPPING);
                }

                free_page_table(pgtbl);
                lab_check(ok, "Map & unmap huge range");
        }
        {
                bool ok = true;
                void *pgtbl = get_pages(0);
                memset(pgtbl, 0, PAGE_SIZE);
                paddr_t pa;
                pte_t *pte;
                int ret;
                /* 1GB + 4MB + 40KB */
                size_t len = (1 << 30) + (4 << 20) + 10 * PAGE_SIZE;
                size_t free_mem, used_mem;

                free_mem = get_free_mem_size_from_buddy(&global_mem[0]);
                ret = map_range_in_pgtbl_huge(
                        pgtbl, 0x100000000, 0x100000000, len, flags);
                lab_assert(ret == 0);
                used_mem =
                        free_mem - get_free_mem_size_from_buddy(&global_mem[0]);
                lab_assert(used_mem < PAGE_SIZE * 8);

                for (vaddr_t va = 0x100000000; va < 0x100000000 + len;
                     va += 5 * PAGE_SIZE + 0x100) {
                        ret = query_in_pgtbl(pgtbl, va, &pa, &pte);
                        lab_assert(ret == 0 && pa == va);
                }

                ret = unmap_range_in_pgtbl_huge(pgtbl, 0x100000000, len);
                lab_assert(ret == 0);

                for (vaddr_t va = 0x100000000; va < 0x100000000 + len;
                     va += 5 * PAGE_SIZE + 0x100) {
                        ret = query_in_pgtbl(pgtbl, va, &pa, &pte);
                        lab_assert(ret == -ENOMAPPING);
                }

                free_page_table(pgtbl);
                lab_check(ok, "Map & unmap with huge page support");
        }
        printk("[TEST] Page table tests finished\n");
}
#endif /* CHCORE_KERNEL_TEST */
