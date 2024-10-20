#pragma once

#include <aarch64/mmu.h>

struct pgdir {
    PTEntriesPtr pt;
};//表示页表目录的结构体

void init_pgdir(struct pgdir *pgdir);
void free_pgdir(struct pgdir *pgdir);
void attach_pgdir(struct pgdir *pgdir);
