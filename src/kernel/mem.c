#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>
// #include <kernel/printk.h>

RefCount kalloc_page_cnt;

// 用于追踪空闲页的链表节点结构
typedef struct FreePage {
    struct FreePage* next;  // 指向下一个空闲页
} FreePage;

static FreePage* free_page_list = NULL;  // 空闲页链表
static SpinLock mem_lock;  // 用于保护空闲页链表的锁

// 初始化内存分配器
void kinit() {
    // 初始化引用计数器
    init_rc(&kalloc_page_cnt);

    // 初始化内存锁
    init_spinlock(&mem_lock);

    // 获取内核结束地址（即可用内存的起始地址）
    extern char end[];
    u64 start = PAGE_BASE(P2K((u64)end));  // 内核结束地址对齐到页面边界
    u64 end_addr = PHYSTOP;  // 可用物理内存的结束地址

    // 将从 `end` 到 `PHYSTOP` 的物理内存按页加入到空闲页链表中
    for (u64 addr = start; addr + PAGE_SIZE <= end_addr; addr += PAGE_SIZE) {
        FreePage* page = (FreePage*)addr;
        page->next = free_page_list;  // 插入到空闲页链表的头部
        free_page_list = page;
    }
}

// 分配一个物理页
void* kalloc_page() {
    acquire_spin_lock(&mem_lock);  // 加锁，确保线程安全

    // 如果没有可用的空闲页，返回 NULL
    if (free_page_list == NULL) {
        release_spin_lock(&mem_lock);
        return NULL;
    }

    // 从空闲链表中取出一个页
    FreePage* page = free_page_list;
    free_page_list = page->next;

    // 更新引用计数
    increment_rc(&kalloc_page_cnt);

    release_spin_lock(&mem_lock);  // 解锁

    // 返回分配的物理页地址
    return (void*)page;
}

// 释放物理页
void kfree_page(void* p) {
    if (p == NULL) {
        return;  // 如果释放的指针为空，直接返回
    }

    acquire_spin_lock(&mem_lock);  // 加锁，确保线程安全

    // 将物理页重新加入到空闲链表中
    FreePage* page = (FreePage*)p;
    page->next = free_page_list;
    free_page_list = page;

    // 更新引用计数
    decrement_rc(&kalloc_page_cnt);

    release_spin_lock(&mem_lock);  // 解锁
}