#include <kernel/pt.h>
#include <kernel/mem.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <kernel/mem.h>
#include <kernel/pt.h>
#include <kernel/paging.h>

#define CHECK_DESCRIPTOR(entry) ((entry) & 0x1)
#define VA_STOP 0xFFFFFFFFFFFF

int debug_pt = 0;
PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc)
{
    // if(debug_pt)printk("get_pte\n");
    // TODO:
    // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
    // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or return NULL if false.
    // THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED BY PTE.
    //解析多级页表
    PTEntriesPtr pt0 = NULL;
    PTEntriesPtr pt1 = NULL;
    PTEntriesPtr pt2 = NULL;
    PTEntriesPtr pt3 = NULL;
    // 如果某一级页表不存在，则停止查找，跳转至分配页表的逻辑。
    if((pt0 = pgdir->pt) != NULL){
        if(pt0[VA_PART0(va)] & PTE_VALID){
            pt1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt0[VA_PART0(va)]));
            if(pt1[VA_PART1(va)] & PTE_VALID){
                pt2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt1[VA_PART1(va)]));
                if(pt2[VA_PART2(va)] & PTE_VALID){
                    pt3 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt2[VA_PART2(va)]));
                    return &pt3[VA_PART3(va)];
                }
            }
        }
    }
    //如果 alloc 为 true，且某一级页表缺失；分配新的页表
    if(alloc){
        // printk("alloc page\n");
        if(pt0 == NULL){
            pgdir->pt = pt0 = kalloc_page();
            memset(pt0, 0, PAGE_SIZE);
        }
        if(pt1 == NULL){
            pt1 = kalloc_page();
            memset(pt1, 0, PAGE_SIZE);
            pt0[VA_PART0(va)] = K2P(pt1)| PTE_TABLE | PTE_VALID;
        }
        if(pt2 == NULL){
            pt2 = kalloc_page();
            memset(pt2, 0, PAGE_SIZE);
            pt1[VA_PART1(va)] = K2P(pt2)| PTE_TABLE | PTE_VALID;
        }
        if(pt3 == NULL){
            pt3 = kalloc_page();
            memset(pt3, 0, PAGE_SIZE);
            pt2[VA_PART2(va)] = K2P(pt3)| PTE_TABLE | PTE_VALID;
        }
        return &pt3[VA_PART3(va)];
    }

    return NULL;
}

void init_pgdir(struct pgdir *pgdir)
{
    pgdir->pt = NULL;
    
}

void free_pgdir(struct pgdir *pgdir)
{
    if(debug_pt)printk("free_pgdir on CPU %lld\n",cpuid());
    // TODO:
    // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
    // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE
    //判断每一级页表是否指向下一级页表，由高级页表开始释放
    if(pgdir->pt != NULL){
        PTEntriesPtr pt0 = pgdir->pt;
        for(int i = 0; i < N_PTE_PER_TABLE; i++){
            if(pt0[i] & PTE_VALID){
                PTEntriesPtr pt1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt0[i]));
                for(int j = 0; j < N_PTE_PER_TABLE; j++){
                    if(pt1[j] & PTE_VALID){
                        PTEntriesPtr pt2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt1[j]));
                        for(int k = 0; k < N_PTE_PER_TABLE; k++){
                            if(pt2[k] & PTE_VALID) kfree_page((void *)P2K(PTE_ADDRESS(pt2[k])));
                        }
                        kfree_page(pt2);
                    } 
                }
                kfree_page(pt1);
            }
        }
        kfree_page(pt0);
        pgdir->pt = NULL;
    }
    // printk("free_pgdir done\n");
}

void attach_pgdir(struct pgdir *pgdir)
{
    // printk("attach_pgdir\n");
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
}


// Copy page table with COW support accroding to the section defs
void copy_pgdir(struct pgdir *src, struct pgdir *dest)
{
    if (!src->pt) {
        return;
    }

    ListNode *node = src->section_head.next;
    // Look for heap section
    while (node != &src->section_head) {
        struct section *section = container_of(node, struct section, stnode);

        u64 page_base = PAGE_BASE(section->begin);
        while (page_base < section->end) {
            PTEntriesPtr src_pte = get_pte(src, page_base, false);
            ASSERT(src_pte != NULL);

            void *phys_page = share_page((void *)P2K(PTE_ADDRESS(*src_pte)));
            vmmap(dest, page_base, phys_page, PTE_USER_DATA | PTE_RO);

            // Change original pte to readonly
            *src_pte |= PTE_RO;
            page_base += PAGE_SIZE;
        }

        node = node->next;
    }

    arch_tlbi_vmalle1is();
}


/**
 * Map virtual address 'va' to the physical address represented by kernel
 * address 'ka' in page directory 'pd', 'flags' is the flags for the page
 * table entry.
 */
void vmmap(struct pgdir *pd, u64 va, void *ka, u64 flags)
{
    /* (Final) TODO BEGIN */

    // TODO
    // Map virtual address 'va' to the physical address represented by kernel
    // address 'ka' in page directory 'pd', 'flags' is the flags for the page
    // table entry

    ASSERT(va % PAGE_SIZE == 0);
    ASSERT((u64)ka % PAGE_SIZE == 0);

    PTEntriesPtr pte = get_pte(pd, va, true);
    ASSERT(pte != NULL);

    // Free the original page if there is
    if ((*pte) & 0x1) {
        // Kernel address of physical page
        void *old_page = (void *)P2K(PTE_ADDRESS(*pte));
        kfree_page(old_page);
    }

    *pte = K2P(ka) | flags;

    // Flush tlb to avoid strange bugs
    arch_tlbi_vmalle1is();

    /* (Final) TODO END */
}

/*
 * Copy len bytes from p to user address va in page table pgdir.
 * Allocate physical pages if required.
 * Useful when pgdir is not the current page table.
 */
int copyout(struct pgdir *pd, void *va, void *p, usize len)
{
    /* (Final) TODO BEGIN */
    char *source = (char *)p;
    u64 va_offset = (u64)va;

    while (len > 0) {
        u64 va_page_base = PAGE_BASE(va_offset);
        PTEntriesPtr pte = get_pte(pd, va_page_base, true);
        if (pte == NULL) {
            return -1;
        }

        // Allocate page if there isn't one
        if (!CHECK_DESCRIPTOR(*pte)) {
            char *new_page = (char *)kalloc_page();
            if (new_page == NULL) {
                return -1;
            }
            *pte = K2P(new_page) | PTE_USER_DATA;
            arch_tlbi_vmalle1is();
        }

        char *page_addr = (char *)P2K(PTE_ADDRESS(*pte));
        u32 offset_in_page = va_offset - va_page_base;
        usize copy_count = MIN(PAGE_SIZE - offset_in_page, len);

        memcpy(page_addr + offset_in_page, source, copy_count);
        len -= copy_count;
        source += copy_count;
        va_offset += copy_count;
    }

    return 0;
    /* (Final) TODO END */
}
