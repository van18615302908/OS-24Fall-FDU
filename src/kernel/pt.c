#include <kernel/pt.h>
#include <kernel/mem.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <kernel/mem.h>
#include <kernel/pt.h>
#include <kernel/paging.h>



PTEntriesPtr allocate_table(PTEntry *parent_level_pte)
{
    PTEntriesPtr new_page_table = kalloc_page();

    memset(new_page_table, 0, PAGE_SIZE);
    if (parent_level_pte) {
        PTEntry descriptor = (PTEntry)(PTEntriesPtr)K2P(new_page_table);
        descriptor |= PTE_PAGE;
        *parent_level_pte = descriptor;
    }
    return new_page_table;
}

PTEntriesPtr get_or_alloc_table(PTEntriesPtr parent_table, u64 index, bool alloc) {
    if (!CHECK_DESCRIPTOR(parent_table[index])) {
        if (alloc) {
            return allocate_table(parent_table + index);
        }
        return NULL;
    }
    return (PTEntriesPtr)P2K(PTE_ADDRESS(parent_table[index]));
}

PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc) {
    // 顶层页表指针
    PTEntriesPtr pt_l0 = pgdir->pt;
    if (!pt_l0) {
        if (alloc) {
            pt_l0 = pgdir->pt = allocate_table(NULL);
            if (!pt_l0) return NULL; // 分配失败
        } else {
            return NULL;
        }
    }

    // 逐级解析页表
    u64 l0_index = VA_PART0(va);
    PTEntriesPtr pt_l1 = get_or_alloc_table(pt_l0, l0_index, alloc);
    if (!pt_l1) return NULL;

    u64 l1_index = VA_PART1(va);
    PTEntriesPtr pt_l2 = get_or_alloc_table(pt_l1, l1_index, alloc);
    if (!pt_l2) return NULL;

    u64 l2_index = VA_PART2(va);
    PTEntriesPtr pt_l3 = get_or_alloc_table(pt_l2, l2_index, alloc);
    if (!pt_l3) return NULL;

    // 最终返回 PTE 指针
    u64 l3_index = VA_PART3(va);
    return pt_l3 + l3_index;
}

void init_pgdir(struct pgdir *pgdir)
{
    init_spinlock(&pgdir->lock);
    pgdir->pt = kalloc_page();
    memset(pgdir->pt, 0, PAGE_SIZE);
}

void free_pgdir(struct pgdir *pgdir)
{
    // TODO:
    // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
    // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE
    if (!pgdir->pt) {
        return;
    }

    for (int i0 = 0; i0 < N_PTE_PER_TABLE; i0++) {
        if (!CHECK_DESCRIPTOR(pgdir->pt[i0])) {
            continue;
        }

        PTEntriesPtr pt_l1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pgdir->pt[i0]));
        for (int i1 = 0; i1 < N_PTE_PER_TABLE; i1++) {
            if (!CHECK_DESCRIPTOR(pt_l1[i1])) {
                continue;
            }

            PTEntriesPtr pt_l2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt_l1[i1]));
            for (int i2 = 0; i2 < N_PTE_PER_TABLE; i2++) {
                if (!CHECK_DESCRIPTOR(pt_l2[i2])) {
                    continue;
                }

                void *pt_l3 = (void *)P2K(PTE_ADDRESS(pt_l2[i2]));
                kfree_page(pt_l3);
            }

            kfree_page(pt_l2);
        }

        kfree_page(pt_l1);
    }

    kfree_page(pgdir->pt);
    pgdir->pt = NULL;
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

            // Skip unallocated pages
            if (src_pte && CHECK_DESCRIPTOR(*src_pte)) {
                void *phys_page =
                        share_page((void *)P2K(PTE_ADDRESS(*src_pte)));
                vmmap(dest, page_base, phys_page, PTE_USER_DATA | PTE_RO);

                // Change original pte to readonly
                *src_pte |= PTE_RO;
            }

            page_base += PAGE_SIZE;
        }

        node = node->next;
    }

    arch_tlbi_vmalle1is();
}

void attach_pgdir(struct pgdir *pgdir)
{
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
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

char* get_or_alloc_physical_page(struct pgdir *pd, u64 va_page_base) {
    // 获取页表项
    PTEntriesPtr pte = get_pte(pd, va_page_base, true);
    if (pte == NULL) {
        printk("Failed to get PTE for VA: %llu\n", va_page_base);
        return NULL;
    }

    // 如果页表项无效，则分配物理页
    if (!CHECK_DESCRIPTOR(*pte)) {
        char *new_page = (char *)kalloc_page();
        if (new_page == NULL) {
            printk("Failed to allocate physical page for VA: %llu\n", va_page_base);
            return NULL;
        }

        // 设置页表项并刷新 TLB
        *pte = K2P(new_page) | PTE_USER_DATA;
        arch_tlbi_vmalle1is();
    }

    // 返回物理页地址（内核可访问）
    return (char *)P2K(PTE_ADDRESS(*pte));
}
/*
 * Copy len bytes from p to user address va in page table pgdir.
 * Allocate physical pages if required.
 * Useful when pgdir is not the current page table.
 */
int copyout(struct pgdir *pd, void *va, void *p, usize len) {
    char *source = (char *)p;
    u64 va_offset = (u64)va;

    while (len > 0) {
        u64 va_page_base = PAGE_BASE(va_offset);

        // 获取或分配页表项
        char *page_addr = get_or_alloc_physical_page(pd, va_page_base);
        if (page_addr == NULL) {
            printk("Failed to allocate or access page for VA: %llu\n", va_page_base);
            return -1;
        }

        // 计算当前页中可以拷贝的字节数
        u32 offset_in_page = va_offset - va_page_base;
        usize copy_count = MIN(PAGE_SIZE - offset_in_page, len);

        // 拷贝数据到目标页
        memcpy(page_addr + offset_in_page, source, copy_count);

        // 更新剩余长度和地址偏移
        len -= copy_count;
        source += copy_count;
        va_offset += copy_count;
    }

    return 0;
}

/*
 * Copy len bytes from inode ip (from given offset) to user address va. 
 * Name of this function is taken from xv6
 */
int load_uvm(struct pgdir *pd, u64 va, Inode *ip, usize offset, usize len)
{
    usize bytes_loaded = 0;
    u64 va_pos = va;
    while (bytes_loaded < len) {
        char *new_page = kalloc_page();
        memset(new_page, 0, PAGE_SIZE);

        u64 va_page_base = PAGE_BASE(va_pos);
        u64 va_offset_in_page = va_pos - va_page_base;
        u32 read_count = MIN(PAGE_SIZE - va_offset_in_page, len - bytes_loaded);
        if (inodes.read(ip, (u8*)new_page + va_offset_in_page, offset, read_count) !=
            read_count) {
            printk("(warn) read failure when loading uvm\n");
            return -1;
        }
        vmmap(pd, va_page_base, new_page, PTE_USER_DATA);

        bytes_loaded += read_count;
        offset += read_count;
        va_pos += read_count;
    }

    return 0;
}
