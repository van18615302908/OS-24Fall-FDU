#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>
#include <common/list.h>
#include <kernel/printk.h>
#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
RefCount kalloc_page_cnt;

int debug = 0;

#define MAX_SIZE 1000

// 定义一个空闲页结构，包含 ListNode
typedef struct FreePage {
    ListNode node;  // 使用 ListNode 来管理空闲页
} FreePage;




//储存用slab
typedef struct slab
{
    struct slab* next; // 指向下一个内存块
}slab;


//储存slab的链表
typedef struct slabs{
    int size;
    int num;
    struct slabs* next;
    struct slab* slab_node;
}slabs;

//储存页的数组
typedef struct SlabList{
    struct Slabs* head;
} SlabList;

SlabList slabs_list_glo[MAX_SIZE];

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
    char* page = (char*)ALIGN_UP((unsigned long)end, PAGE_SIZE);// 从 end 开始，向上对齐到 PAGE_SIZE


    for (; page + PAGE_SIZE <= phystop_vaddr; page += PAGE_SIZE) {
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


    ListNode* node = _detach_from_list(free_pages_list.next);  // 先移除节点

    FreePage* page = (FreePage*)node;  // 将移除的节点转换为 FreePage 类型


    // 检查页面地址是否对齐
    if ((u64)page & (PAGE_SIZE - 1)) {
        release_spinlock(&mem_lock);
        // printk("kalloc_page:返回未对齐的页面地址 %p\n", page);
        return NULL;
    }
    // 打印当前和接下来的十个next元素
    if (debug)
    {
        ListNode* current = free_pages_list.next;
        printk("kalloc_page_distribution: free_pages_list.next=%p\n", current);

        for (int i = 0; i < 10 && current != &free_pages_list; i++) {
            current = current->next;
            printk("Next element %d: %p %p\n", i + 1, current, &current);
        }
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

    // 对齐大小，确保最小对齐到 8 字节
    size = ALIGN_UP(size, 8);
    int size_need = size;//需要的大小

    // 获取自旋锁，防止并发问题
    acquire_spinlock(&mem_lock_block);

    start_loop:
    slabs* new_slabs = (slabs*)slabs_list_glo[size_need / 8 - 1].head;
    start_inner_loop:
    
    while (new_slabs) {

        if(new_slabs->size != size_need){
            //大小不匹配
            new_slabs = new_slabs->next;
            goto start_inner_loop;
        }

        slab* current_slab = new_slabs->slab_node;

        if(current_slab){
            new_slabs->slab_node = current_slab->next;
            release_spinlock(&mem_lock_block);
            new_slabs->num--;
            // printk("kalloc 成功: %d\n", size_need);
            return current_slab;
        }
        //这一页没有空闲slab
        new_slabs = new_slabs->next;
        goto start_inner_loop;
    }

    //没有合适的slabs，需要重新分配
    void* page = kalloc_page();
    // printk("kalloc_page: %p\n", page);
    debug = 0;
    if (!page) {
        return NULL;  // 分配失败
    }


    //初始化slabs
    new_slabs = (slabs*)page;
    // new_slabs->next = slabs_list;
    new_slabs->size = size_need;

    slab* head = (slab*)((char*)new_slabs + sizeof(slabs));
    slab* current = head;
    int k = 0;
    while ((char*)current + 2*size_need < (char*)page + PAGE_SIZE) {
        current->next = (slab*)((char*)current + size_need);
        current = current->next;
        k++;
    }

    current->next = NULL; // 确保链表的最后一个节点指向 NULL
    new_slabs->slab_node = head;
    new_slabs->num = k;
    

    //将slabs插入到slabs_list
    int index = size_need / 8 - 1;
    
    slabs* slps_head = (slabs*)slabs_list_glo[index].head;
    new_slabs->next = slps_head; 
    slabs_list_glo[index].head = (struct Slabs *)new_slabs;

    goto start_loop;

    return NULL;

}




void kfree(void* ptr) {

    if (!ptr) {
        // 释放空指针，直接返回
        return;
    }

    // 获取自旋锁，防止并发问题
    acquire_spinlock(&mem_lock_block);

    slabs* return_slabs = (slabs*)(round_down((u64)ptr, PAGE_SIZE));

    slab* head = return_slabs->slab_node;
    slab* current = (slab*)ptr;
    current->next = head;
    return_slabs->slab_node = current;
    return_slabs->num++;
    if(return_slabs->num == (int)(PAGE_SIZE - sizeof(return_slabs))/return_slabs->size - 1){
        //这一页的slab全部被释放
        slabs* current_slabs = (slabs*)slabs_list_glo[return_slabs->size / 8 - 1].head;
        slabs* pre_slabs = NULL;
        while (current_slabs)
        {
            if(current_slabs == return_slabs){
                if(pre_slabs){
                    pre_slabs->next = current_slabs->next;
                }else{
                    slabs_list_glo[return_slabs->size / 8 - 1].head = (struct Slabs *)current_slabs->next;
                }
                break;
            }
            pre_slabs = current_slabs;
            current_slabs = current_slabs->next;
        }
        kfree_page(return_slabs);
    }

    // 释放自旋锁
    release_spinlock(&mem_lock_block);

}