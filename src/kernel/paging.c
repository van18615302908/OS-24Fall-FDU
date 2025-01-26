#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/string.h>
#include <fs/block_device.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>

void init_sections(ListNode *section_head)
{
    /* (Final) TODO BEGIN */
    init_list_node(section_head);
    /* (Final) TODO END */
}

void free_sections(struct pgdir *pd)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pd->lock);
    ListNode *node = pd->section_head.next;
    while (node != &pd->section_head) {
        struct section *section = container_of(node, struct section, stnode);
        ListNode *next = node->next;

        if (section->flags == ST_MMAP_SHARED &&
            (section->prot & 2 /* PROT_WRITE */)) {
            write_back(pd, section->fp, section->begin, section->offset,
                       section->length);
        }

        if (section->fp) {
            file_close(section->fp);
        }

        _detach_from_list(node);
        kfree(section);
        node = next;
    }
    release_spinlock(&pd->lock);
    /* (Final) TODO END */
}

u64 sbrk(i64 size)
{
    /**
     * (Final) TODO BEGIN 
     * 
     * Increase the heap size of current process by `size`.
     * If `size` is negative, decrease heap size. `size` must
     * be a multiple of PAGE_SIZE.
     * 
     * Return the previous heap_end.
     */

    Proc *this = thisproc();

    ListNode *node = this->pgdir.section_head.next;
    struct section *heap_section = NULL;
    while (node != &this->pgdir.section_head) {
        struct section *section = container_of(node, struct section, stnode);
        if (section->flags & ST_HEAP) {
            heap_section = section;
            break;
        }

        node = node->next;
    }

    if (heap_section == NULL) {
        return -1;
    }

    if (heap_section->end + size < heap_section->begin) {
        return -1;
    }

    u64 original_end = heap_section->end;
    heap_section->end += size;

    if (size < 0) {
        u64 free_pages_start = PAGE_BASE((heap_section->end + (PAGE_SIZE - 1)));
        u64 free_pages_end = PAGE_BASE((original_end - 1));
        for (u64 page_addr = free_pages_start; page_addr <= free_pages_end;
             page_addr += PAGE_SIZE) {
            PTEntriesPtr pte = get_pte(&this->pgdir, page_addr, false);
            if (pte && (*pte) & 0x1) {
                void *physical_page = (void *)P2K(PTE_ADDRESS(*pte));
                kfree_page(physical_page);
                *pte = 0;
            }
        }
        arch_tlbi_vmalle1is();
    }

    return original_end;
    /* (Final) TODO END */
}


struct section* find_section_containing_addr(struct pgdir *pgdir, u64 addr) {
    acquire_spinlock(&pgdir->lock);
    ListNode *node = pgdir->section_head.next;

    while (node != &pgdir->section_head) {
        struct section *sec = container_of(node, struct section, stnode);
        if (sec->begin <= addr && addr < sec->end) {
            release_spinlock(&pgdir->lock);
            return sec;
        }
        node = node->next;
    }

    release_spinlock(&pgdir->lock);
    return NULL; // 未找到段
}
int handle_translation_fault(struct pgdir *pd, struct section *sec, u64 page_addr) {
    if (!(sec->flags & (ST_HEAP | ST_STACK)) && !sec->fp) {
        printk("(warn) translation fault not resolvable.\n");
        return -1;
    }

    void *new_page = kalloc_page();
    if (!new_page) {
        return -1;
    }

    vmmap(pd, page_addr, new_page, PTE_USER_DATA);

    if (sec->fp) { // 文件映射段
        u64 flags = PTE_USER_DATA;
        if ((sec->flags & ST_MMAP) && sec->prot == 1) {
            flags |= PTE_RO;
        }
        map_file(pd, sec->fp, sec->begin, sec->offset, sec->length, flags);
    }

    return 0; // 成功处理
}
int handle_permission_fault(struct pgdir *pd, struct section *sec, u64 addr) {
    if ((sec->flags & ST_MMAP) && sec->prot == 1) {
        printk("(warn) attempting to write readonly mmap.\n");
        return -1;
    }

    PTEntriesPtr pte = get_pte(pd, addr, false);
    if (!pte || !(*pte & 0x1)) {
        printk("Invalid PTE for address %llu\n", addr);
        return -1;
    }

    void *old_page = (void *)P2K(PTE_ADDRESS(*pte));
    void *new_page = kalloc_page();
    if (!new_page) {
        return -1;
    }

    memcpy(new_page, old_page, PAGE_SIZE);
    vmmap(pd, PAGE_BASE(addr), new_page, PTE_USER_DATA);
    return 0;
}

int pgfault_handler(u64 iss)
{
    Proc *p = thisproc();
    struct pgdir *pd = &p->pgdir;
    u64 addr = arch_get_far(); // 获取引发缺页的地址

    // 检查 `far` 有效性
    if ((iss << 10) & 0x1) {
        printk("ERROR: Invalid FAR, cannot handle.\n");
        return -1;
    }

    // 查找包含缺页地址的段
    struct section *section = find_section_containing_addr(&p->pgdir, addr);
    if (!section) {
        printk("No section contains address %llu, killing proc %d\n", addr, p->pid);
        return -1;
    }

    u64 page_addr = PAGE_BASE(addr);
    const u64 fault_type = (iss & 0x3F) >> 2;

    // 根据缺页类型处理
    switch (fault_type) {
        case 0x1: // Translation fault
            if (handle_translation_fault(pd, section, page_addr) == 0) {
                return 0; // 处理成功
            }
            break;
        case 0x3: // Permission fault
            if (handle_permission_fault(pd, section, addr) == 0) {
                return 0; // 处理成功
            }
            break;
        default:
            printk("Unknown fault type %llu, killing proc %d\n", fault_type, p->pid);
    }

    // 无法处理的情况
    return -1;
}

void copy_sections(ListNode *from_head, ListNode *to_head)
{
    /* (Final) TODO BEGIN */
    ListNode *node = from_head->next;
    while (node != from_head) {
        struct section *section = container_of(node, struct section, stnode);

        struct section *copied = kalloc(sizeof(struct section));
        copied->begin = section->begin;
        copied->end = section->end;
        copied->flags = section->flags;
        copied->fp = NULL;
        if (section->fp) {
            copied->fp = file_dup(section->fp);
            copied->offset = section->offset;
            copied->length = section->length;
        }

        _insert_into_list(to_head, &copied->stnode);
        node = node->next;
    }
    /* (Final) TODO END */
}

int map_file(struct pgdir *pd, File *f, u64 va, usize offset, usize len,
             u64 flags)
{
    usize bytes_read = 0;
    u64 va_pos = va;
    f->off = offset;

    while (bytes_read < len) {
        u64 va_page_base = PAGE_BASE(va_pos);
        PTEntriesPtr pte = get_pte(pd, va_page_base, false);

        char *phys_page = NULL;
        if (!pte || !CHECK_DESCRIPTOR(*pte)) {
            phys_page = kalloc_page();
            memset(phys_page, 0, PAGE_SIZE);
            vmmap(pd, va_page_base, phys_page, flags);
        } else {
            phys_page = (char *)P2K(PTE_ADDRESS(*pte));
        }

        u64 va_offset_in_page = va_pos - va_page_base;
        u32 should_read = MIN(PAGE_SIZE - va_offset_in_page, len - bytes_read);

        u32 read_count =
                file_read(f, phys_page + va_offset_in_page, should_read);

        bytes_read += read_count;
        va_pos += read_count;
        if (read_count != should_read) {
            va_page_base = PAGE_BASE(va_pos);
            va_offset_in_page = va_pos - va_page_base;

            // Fill rest of this page with zero
            if (va_pos % PAGE_SIZE != 0) {
                u64 zero_count = ALIGN_UP(va_pos, PAGE_SIZE) - va_pos;
                memset(phys_page + va_offset_in_page, 0, zero_count);
                va_pos += zero_count;
            }

            ASSERT(va_pos % PAGE_SIZE == 0);
            // Map the rest to shared zero page
            while (va_pos < va + len) {
                /* code */
                vmmap(pd, va_pos, get_zero_page(), PTE_USER_DATA | PTE_RO);
                va_pos += PAGE_SIZE;
            }

            return bytes_read;
        }
    }

    return bytes_read;
}

int write_back(struct pgdir *pd, File *f, u64 va, usize offset, usize len)
{
    usize bytes_written = 0;
    u64 va_pos = va;
    f->off = offset;

    while (bytes_written < len) {
        u64 va_page_base = PAGE_BASE(va_pos);
        PTEntriesPtr pte = get_pte(pd, va_page_base, false);

        if (!pte || !CHECK_DESCRIPTOR(*pte)) {
            return 0;
        }

        char *phys_addr = (char *)P2K(PTE_ADDRESS(*pte));

        u64 va_offset_in_page = va_pos - va_page_base;
        u32 write_count =
                MIN(PAGE_SIZE - va_offset_in_page, len - bytes_written);

        if (file_write(f, phys_addr + va_offset_in_page, write_count) !=
            write_count) {
            return -1;
        }

        bytes_written += write_count;
        va_pos += write_count;
    }

    return bytes_written;
}
