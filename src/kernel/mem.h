#pragma once
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/rc.h>

#define PAGE_COUNT ((P2K(PHYSTOP) - PAGE_BASE((u64) & end)) / PAGE_SIZE - 1)
#define MAX_PAGE_COUNT 262000
struct page {
    RefCount ref;
};

void kinit();
u64 left_page_cnt();

WARN_RESULT void *kalloc_page();
void kfree_page(void *);

WARN_RESULT void *kalloc(unsigned long long);
void kfree(void *);
void* share_page(void *);

WARN_RESULT void *get_zero_page();
typedef struct __page_header {
    struct __page_header *next, *prev;
    int filled_blocks;
    // u16 id;
    int tier;
    char *free_block;
    // bool allocated;
} page_header;
