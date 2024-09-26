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
static SpinLock mem_lock_block;

extern char end[];  // 内核结束地址，空闲页从此地址之后开始



void kinit() {
    init_rc(&kalloc_page_cnt);  // 初始化页面计数器，！不许修改！

    init_spinlock(&mem_lock);   // 初始化自旋锁
    init_list_node(&free_pages_list);  // 初始化空闲页链表头

    // 将 PHYSTOP 转换为内核虚拟地址，这样就可以与 page 比较
    char* phystop_vaddr = (char*)P2K(PHYSTOP);


    // 初始化空闲页链表
    // char* page = (char*)end;
    char* page = (char*)ALIGN_UP((unsigned long)end, PAGE_SIZE);// 从 end 开始，向上对齐到 PAGE_SIZE

    // printk("kinit: end=%p, page=%p\n", end, page);

    for (; page + PAGE_SIZE <= phystop_vaddr; page += PAGE_SIZE) {
        kfree_page(page);  // 将每个页面放入空闲链表中
    }

}

void* kalloc_page() {
    acquire_spinlock(&mem_lock);  // 获取自旋锁，防止并发问题

    if (free_pages_list.next == &free_pages_list) {
        // 空闲链表为空，无法分配页面
        // printk("kalloc_page: no free pages\n");
        release_spinlock(&mem_lock);
        return NULL;
    }



    // // 打印当前和接下来的十个next元素
    // if (1)
    // {
    //     ListNode* current = free_pages_list.next;
    //     printk("kalloc_page_distribution: free_pages_list.next=%p\n", current);

    //     for (int i = 0; i < 10 && current != &free_pages_list; i++) {
    //         current = current->next;
    //         printk("Next element %d: %p %p\n", i + 1, current, &current);
    //     }
    // }


    ListNode* node = _detach_from_list(free_pages_list.next);  // 先移除节点

    FreePage* page = (FreePage*)node;  // 将移除的节点转换为 FreePage 类型
    
    // printk("kalloc_page: page=%p\n", page);
    // 检查页面地址是否对齐
    if ((u64)page & (PAGE_SIZE - 1)) {
        release_spinlock(&mem_lock);
        // printk("kalloc_page:返回未对齐的页面地址 %p\n", page);
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



//切分版本
void* kalloc(unsigned long long size) {
    // 获取自旋锁，防止并发问题
    acquire_spinlock(&mem_lock_block);

    size += sizeof(MemoryBlock);  // 加上 MemoryBlock 的大小
    // printk("！！！！！！！！!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!kalloc: size=%llu\n", size);
    // 对齐大小，确保最小对齐到 8 字节
    size = ALIGN_UP(size, 8);

    // 遍历现有的内存池，寻找合适的块
    MemoryPool* pool = memory_pool_list;
    while (pool) {
        // printk("\n kalloc: size=%llu， page = %p \n", size,(void*)pool);
        MemoryBlock* prev_block = NULL;
        MemoryBlock* block = pool->free_list;


        // 遍历当前内存池的空闲列表
        while (block) {
            // printk("kalloc: size=%llu, block=%p,block_size=%llu\n", size, block,block->size);
            if (block->size >= size && block->size >= 32) {
                // 找到合适的块，分配内存
                //只有当块比需要的内存大得多时才切分块
                if ( block->size  > 16 + size) {
                    // 如果块比需要的内存大得多，切分块
                    MemoryBlock* new_block = (MemoryBlock*)((char*)(block ) + size);
                    new_block->size = block->size - size - sizeof(MemoryBlock);
                    new_block->next = block->next;

                    // 更新当前块的大小和空闲列表
                    block->size = size;
                    if (prev_block) {
                        prev_block->next = new_block;
                    } else {
                        pool->free_list = new_block;
                    }
                } else {
                    // 否则直接分配整个块
                    if (prev_block) {
                        prev_block->next = block->next;
                    } else {
                        pool->free_list = block->next;
                    }
                }

                // 释放自旋锁
                release_spinlock(&mem_lock_block);
                // printk("kalloc: 成功\n");
                return (void*)(block + 1);  // 返回块之后的实际数据地址
            }
            prev_block = block;
            block = block->next;
        }

        pool = pool->next;
        // printk("next page\n");
    }
    // printk("没能找到合适的块，分配新的页作为内存池\n");
    // printk("kalloc_page()启动\n");
    // 如果没有找到合适的块，分配新的页作为内存池
    void* page = kalloc_page();
    printk("kalloc_page()成功\n");
    
    if (!page) {
        // printk("kalloc: failed to allocate page\n");
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

    // 释放自旋锁
    release_spinlock(&mem_lock);


    // 递归调用 kalloc 再次分配内存
    // printk("kalloc: 递归调用\n");
    return kalloc(size);
}



void kfree(void* ptr) {

    if (!ptr) {
        // 释放空指针，直接返回
        return;
    }

    // 获取自旋锁，防止并发问题
    acquire_spinlock(&mem_lock_block);

    // 获取指向 MemoryBlock 的指针
    MemoryBlock* block = (MemoryBlock*)ptr - 1;

    // 遍历内存池，找到该块所属的内存池
    MemoryPool* pool = memory_pool_list;
    while (pool) {
        char* pool_start = (char*)pool;
        char* pool_end = pool_start + PAGE_SIZE;

        if ((char*)block >= pool_start && (char*)block < pool_end) {
            // 找到所属内存池，准备将块插入空闲列表
            MemoryBlock* prev = NULL;
            MemoryBlock* curr = pool->free_list;

            // 寻找空闲块的位置，保持空闲块的地址有序
            while (curr && (char*)curr < (char*)block) {
                prev = curr;
                curr = curr->next;
            }

            // 尝试与前一个空闲块合并
            if (prev && (char*)prev + prev->size + sizeof(MemoryBlock) == (char*)block) {
                // 可以与前一个块合并
                prev->size += block->size + sizeof(MemoryBlock);
                block = prev;
            } else {
                // 不能合并，将块插入到当前位置
                block->next = curr;
                if (prev) {
                    prev->next = block;
                } else {
                    pool->free_list = block;
                }
            }

            // 尝试与后一个空闲块合并
            if (curr && (char*)block + block->size + sizeof(MemoryBlock) == (char*)curr) {
                // 可以与后一个块合并
                block->size += curr->size + sizeof(MemoryBlock);
                block->next = curr->next;
            }

            // 释放自旋锁
            release_spinlock(&mem_lock_block);
            return;
        }

        pool = pool->next;
    }

    // 释放自旋锁
    release_spinlock(&mem_lock_block);
    // 如果没有找到所属的池，说明释放有误
    printk("kfree: invalid pointer %p\n", ptr);
}