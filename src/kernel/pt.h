#pragma once

#include <aarch64/mmu.h>
#include <common/spinlock.h>
#include <common/list.h>

struct pgdir {
    PTEntriesPtr pt;
    SpinLock lock;
    ListNode section_head;
};//表示页表目录的结构体

#define STACK_PAGE_COUNT 20
#define STACK_BOTTOM_RESERVED 128
#define ALIGN_UP(addr, size) (((usize)(addr) + (size - 1)) & (-size))
#define ALIGN_DOWN(addr, size) (((usize)(addr)) & (-size))
#define VA_STOP 0xFFFFFFFFFFFF

void init_pgdir(struct pgdir *pgdir);
WARN_RESULT PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc);
void free_pgdir(struct pgdir *pgdir);
void copy_pgdir(struct pgdir *src, struct pgdir *dest);
void attach_pgdir(struct pgdir *pgdir);
void vmmap(struct pgdir *pd, u64 va, void *ka, u64 flags);
int copyout(struct pgdir *pd, void *va, void *p, usize len);