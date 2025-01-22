#pragma once

#include <aarch64/mmu.h>
#include <kernel/proc.h>


#define CHECK_DESCRIPTOR(entry) ((entry) & 0x1)

#define ST_FILE  1                  // File-backed
#define ST_SWAP  (1<<1)             // Unused
#define ST_RO    (1<<2)             // Read-only
#define ST_HEAP  (1<<3)             // Section is heap
#define ST_TEXT  (ST_FILE | ST_RO)  // Section is text
#define ST_DATA  ST_FILE            // Section is data
#define ST_BSS   ST_FILE            // Section is bss
#define ST_STACK (1<<4)             // Section is stack
#define ST_MMAP  (1<<5)
#define ST_MMAP_PRIVATE ST_MMAP
#define ST_MMAP_SHARED  (ST_MMAP | ST_RO)

struct section {
    u64 flags;
    u64 begin;
    u64 end;
    ListNode stnode;
    SleepLock sleeplock;//用于同步

    /* The following fields are for the file-backed sections. */

    struct file *fp;
    u64 offset; // Offset in file
    u64 length; // Length of mapped content in file
    u64 prot; // For mmap uses
};

int pgfault_handler(u64 iss);
void init_sections(ListNode *section_head);
void free_sections(struct pgdir *pd);
void copy_sections(ListNode *from_head, ListNode *to_head);
u64 sbrk(i64 size);
int map_file(struct pgdir *pd, File *f, u64 va, usize offset,
                      usize len, u64 flags);
int write_back(struct pgdir *pd, File *f, u64 va, usize offset,
                      usize len);