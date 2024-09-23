#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>
// #include <kernel/printk.h>

RefCount kalloc_page_cnt;

// 定义一个空闲页链表结构
typedef struct FreePage {
    struct FreePage* next;
} FreePage;

// 空闲页链表的头指针
static FreePage* free_pages_list = NULL;

// 自旋锁，防止并发问题
static SpinLock mem_lock;

extern char end[];  // 内核结束地址，空闲页从此地址之后开始

void kinit() {
    init_rc(&kalloc_page_cnt);  // 初始化页面计数器
    init_spinlock(&mem_lock);  // 初始化自旋锁

    // 初始化空闲页链表
    char* page = (char*)end;
    for (; page + PAGE_SIZE <= (char*)PHYSTOP; page += PAGE_SIZE) {
        kfree_page(page);  // 将每个页面放入空闲链表中
    }
}

void* kalloc_page() {
    acquire_spinlock(&mem_lock);  // 获取自旋锁，防止并发问题

    FreePage* page = free_pages_list;  // 从空闲链表取页
    if (page) {
        free_pages_list = page->next;  // 更新链表头
        increment_rc(&kalloc_page_cnt);  // 更新分配页面计数
    }

    release_spinlock(&mem_lock);  // 释放自旋锁
    return (void*)page;
}

void kfree_page(void* p) {
    if (p == NULL) {
        return;
    }

    acquire_spinlock(&mem_lock);  // 获取自旋锁，防止并发问题

    FreePage* page = (FreePage*)p;
    page->next = free_pages_list;  // 将页面重新插入空闲链表
    free_pages_list = page;

    decrement_rc(&kalloc_page_cnt);  // 更新页面计数

    release_spinlock(&mem_lock);  // 释放自旋锁
}

void* kalloc(unsigned long long size) {
    // 简单实现：仅支持以 PAGE_SIZE 为单位的分配
    if (size <= PAGE_SIZE) {
        return kalloc_page();
    }
    return NULL;
}

void kfree(void* ptr) {
    // 简单实现：仅支持以 PAGE_SIZE 为单位的释放
    kfree_page(ptr);
}