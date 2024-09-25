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


//在每个分配的内存块中保存其大小，以便在释放时能够知道如何正确回收。
typedef struct MemoryBlock {
    unsigned long long size;  // 记录内存块的大小
    struct MemoryBlock* next; // 指向下一个空闲块
} MemoryBlock;

//每个物理页会被视为内存池，池中包含多个 MemoryBlock。在池内进行分配和释放时，需要管理这些块。
typedef struct MemoryPool {
    MemoryBlock* free_list;  // 空闲块链表
    struct MemoryPool* next; // 指向下一个内存池
} MemoryPool;

static MemoryPool* memory_pool_list = NULL;  // 管理所有的内存池




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


void* kalloc(unsigned long long size) {
    // 对齐大小，确保最小对齐到 8 字节
    size = ALIGN_UP(size, 8);

    // 遍历现有的内存池，寻找合适的块
    MemoryPool* pool = memory_pool_list;
    while (pool) {
        MemoryBlock* prev_block = NULL;
        MemoryBlock* block = pool->free_list;

        // 遍历当前内存池的空闲列表
        while (block) {
            if (block->size >= size) {
                // 找到合适的块，分配内存
                if (prev_block) {
                    prev_block->next = block->next;
                } else {
                    pool->free_list = block->next;
                }
                return (void*)(block + 1);  // 返回块之后的实际数据地址
            }
            prev_block = block;
            block = block->next;
        }

        pool = pool->next;
    }

    // 如果没有找到合适的块，分配新的页作为内存池
    void* page = kalloc_page();
    if (!page) {
        return NULL;  // 分配失败
    }

    // 初始化新的内存池
    pool = (MemoryPool*)page;
    pool->next = memory_pool_list;
    memory_pool_list = pool;

    // 初始化物理页内的内存块
    MemoryBlock* block = (MemoryBlock*)(pool + 1);  // 跳过 MemoryPool 结构
    block->size = PAGE_SIZE - sizeof(MemoryPool);   // 剩余内存的大小
    block->next = NULL;

    // 将新分配的块插入空闲列表
    pool->free_list = block;

    // 递归调用 kalloc 再次分配内存
    return kalloc(size);
}

void kfree(void* ptr) {
    if (!ptr) {
        return;
    }

    // 获取指向 MemoryBlock 的指针
    MemoryBlock* block = (MemoryBlock*)ptr - 1;

    // 遍历内存池，找到该块所属的内存池
    MemoryPool* pool = memory_pool_list;
    while (pool) {
        char* pool_start = (char*)pool;
        char* pool_end = pool_start + PAGE_SIZE;

        if ((char*)block >= pool_start && (char*)block < pool_end) {
            // 将块插入到该池的空闲列表中
            block->next = pool->free_list;
            pool->free_list = block;
            return;
        }

        pool = pool->next;
    }

    // 如果没有找到所属的池，说明释放有误
    printk("kfree: invalid pointer %p\n", ptr);
}