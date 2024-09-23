#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>
#include <common/list.h>
#include <kernel/printk.h>
#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
RefCount kalloc_page_cnt;

// 定义一个空闲页结构，包含 ListNode
typedef struct FreePage {
    ListNode node;  // 使用 ListNode 来管理空闲页
} FreePage;

// 空闲页链表的头指针
static ListNode free_pages_list;

// 自旋锁，防止并发问题
static SpinLock mem_lock;

extern char end[];  // 内核结束地址，空闲页从此地址之后开始




// 从链表尾部开始打印
void print_list_from_tail() {
    // 获取链表的尾节点
    ListNode* node = free_pages_list.prev;
    
    printk("Printing free pages from tail:\n");
    
    // 从尾节点开始，向前遍历，直到链表头部
    for (int i = 0; i < 10 && node != &free_pages_list; i++) {
        FreePage* page = (FreePage*)node;
        printk("Page at address: %p\n", page);
        node = node->prev;
    }
}








void kinit() {
    init_rc(&kalloc_page_cnt);  // 初始化页面计数器，！不许修改！

    init_spinlock(&mem_lock);   // 初始化自旋锁
    init_list_node(&free_pages_list);  // 初始化空闲页链表头

    // 将 PHYSTOP 转换为内核虚拟地址，这样就可以与 page 比较
    char* phystop_vaddr = (char*)P2K(PHYSTOP);


    // 初始化空闲页链表
    // char* page = (char*)end;
    char* page = (char*)ALIGN_UP((unsigned long)end, PAGE_SIZE);// 从 end 开始，向上对齐到 PAGE_SIZE
    printk("kinit: end=%p, page=%p\n", end, page);
    for (; page + PAGE_SIZE <= phystop_vaddr; page += PAGE_SIZE) {
        // printk("kinit: page=%p\n", page);
        kfree_page(page);  // 将每个页面放入空闲链表中
    }

}

void* kalloc_page() {
    acquire_spinlock(&mem_lock);  // 获取自旋锁，防止并发问题

    if (free_pages_list.next == &free_pages_list) {
        // 空闲链表为空，无法分配页面
        release_spinlock(&mem_lock);
        return NULL;
    }

    // print_list_from_tail();  // 打印空闲页链表

    // 从空闲链表中取出第一个页面节点
    ListNode* node = _detach_from_list(free_pages_list.next);  // 先移除节点
    FreePage* page = (FreePage*)node;  // 将移除的节点转换为 FreePage 类型
    
    // printk("kalloc_page: page=%p\n", page);
    // 检查页面地址是否对齐
    if ((u64)page & (PAGE_SIZE - 1)) {
        release_spinlock(&mem_lock);
        printk("kalloc_page:返回未对齐的页面地址 %p\n", page);
        return NULL;
    }

    increment_rc(&kalloc_page_cnt);  // 更新分配页面计数

    release_spinlock(&mem_lock);  // 释放自旋锁
    return (void*)page;
}

void kfree_page(void* p) {
    if (p == NULL) {
        return;
    }

    acquire_spinlock(&mem_lock);  // 获取自旋锁，防止并发问题

    // 将释放的页面重新插入到空闲链表中
    FreePage* page = (FreePage*)p;
    _insert_into_list(&free_pages_list, &page->node);

    decrement_rc(&kalloc_page_cnt);  // 更新页面计数

    release_spinlock(&mem_lock);  // 释放自旋锁
}

// void* kalloc(unsigned long long size) {
//     // 简单实现：仅支持以 PAGE_SIZE 为单位的分配
//     if (size <= PAGE_SIZE) {
//         return kalloc_page();
//     }
//     return NULL;
// }

// void kfree(void* ptr) {
//     // 简单实现：仅支持以 PAGE_SIZE 为单位的释放
//     kfree_page(ptr);
// }

void* kalloc(unsigned long long size) {
    return NULL;
}

void kfree(void* ptr) {
    return;
}