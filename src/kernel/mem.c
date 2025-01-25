#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <common/string.h>

#define ALIGN_UP_PTR(addr, size) (void *)(((usize)(addr) + (size - 1)) & (-size))
#define ALIGN_DOWN_PTR(addr, size) (void *)(((usize)(addr)) & (-size))

#define MIN_SIZE 8

RefCount kalloc_page_cnt;
static int total_page_cnt;
extern char end[];
static char *page_pool_start;     // 页面池起始地址
static SpinLock page_spinlock, block_spinlock;
static void *zero_page = NULL;

// 页面元信息数组
static struct page pages[MAX_PAGE_COUNT];

// Block sizes, in bytes
const int block_sizes[] = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048 };

// 空闲页面链表
static page_header *free_list = NULL;
// 不同块大小的部分空闲页面链表
static page_header *partial_block_list[9] = { NULL };

void init_pages()
{
    page_pool_start = ALIGN_UP_PTR(end, PAGE_SIZE);
    char *kernel_stop = (char *)P2K(PHYSTOP);
    for (char *i = page_pool_start; i + PAGE_SIZE <= kernel_stop; i += PAGE_SIZE) {
        page_header *p_header = (page_header *)i;

        if (free_list) {
            free_list->prev = p_header;
        }
        p_header->next = free_list;
        free_list = p_header;

        init_rc(&pages[total_page_cnt++].ref);
    }

}

void kinit()
{
    init_rc(&kalloc_page_cnt);
    init_spinlock(&block_spinlock);
    init_spinlock(&page_spinlock);
    init_pages();
}



void *kalloc_page()
{
    acquire_spinlock(&page_spinlock);
    page_header *p_page = free_list;
    if (!p_page) {
        release_spinlock(&page_spinlock);
        return NULL;
    }

    free_list = p_page->next;
    if (free_list) {
        free_list->prev = NULL;
    }
    p_page->next = p_page->prev = NULL;

    u32 page_index = ((char *)p_page - page_pool_start) / PAGE_SIZE;
    ASSERT(pages[page_index].ref.count == 0);
    increment_rc(&pages[page_index].ref);
    increment_rc(&kalloc_page_cnt);
    release_spinlock(&page_spinlock);

    return p_page;
}


static inline u32 get_page_index(void *p) {
    return ((char *)p - page_pool_start) / PAGE_SIZE;
}


static bool can_free_page(u32 page_index) {
    ASSERT(pages[page_index].ref.count > 0); // 引用计数必须大于 0
    decrement_rc(&pages[page_index].ref);
    return pages[page_index].ref.count == 0; // 如果引用数为 0，则可以释放
}


static void reset_and_insert_page(void *p) {
    page_header *p_page = p;

    if (free_list) {
        free_list->prev = p_page;
    }

    p_page->filled_blocks = 0;
    p_page->free_block = NULL;
    p_page->prev = NULL;
    p_page->next = free_list;
    free_list = p_page;

    decrement_rc(&kalloc_page_cnt);
}


void kfree_page(void *p) {
    acquire_spinlock(&page_spinlock);

    u32 page_index = get_page_index(p);
    if (!can_free_page(page_index)) {
        release_spinlock(&page_spinlock); // 如果不能释放，则直接返回
        return;
    }

    ASSERT(p != zero_page); // zero page 不应该被释放
    reset_and_insert_page(p); // 重置页面元数据并插入到 free list

    release_spinlock(&page_spinlock);
}


static void initialize_page_metadata(page_header *p_page, int tier) {
    p_page->tier = tier;
    p_page->free_block = NULL;
    p_page->filled_blocks = 0;
    p_page->next = NULL;
    p_page->prev = NULL;
}

static void insert_page_into_partial_list(page_header *p_page, int tier) {
    if (partial_block_list[tier]) {
        partial_block_list[tier]->prev = p_page;
    }
    p_page->next = partial_block_list[tier];
    partial_block_list[tier] = p_page;
}


static char *align_payload_start(char *start) {
    return ALIGN_UP_PTR(start, 8);
}


static void initialize_free_blocks(page_header *p_page, u64 block_size) {
    char *payload_start = ((char *)p_page) + sizeof(page_header);
    payload_start = align_payload_start(payload_start);

    const char *upper_bound = (char *)p_page + PAGE_SIZE;
    for (char *i = payload_start; i + block_size < upper_bound; i += block_size) {
        *((char **)i) = p_page->free_block;
        p_page->free_block = i;
    }
}


void setup_page(page_header *p_page, int tier) {
    initialize_page_metadata(p_page, tier); // 初始化页面元数据
    insert_page_into_partial_list(p_page, tier); // 插入到部分块列表中
    initialize_free_blocks(p_page, block_sizes[tier]); // 初始化页面中的空闲块
}

// Get the block size tier of the given size
int get_tier(unsigned long long size)
{
    int leading_zeros = __builtin_clzll(size - 1);
    size = 0x8000000000000000 >> (leading_zeros - 1);

    // Map size to tier by `tier = log2(size) - 3` (ctz is a fast equivalent to log2)
    int trailing_zeros = __builtin_ctzll(size);
    return MAX(0, trailing_zeros - 3);
}

void *kalloc(unsigned long long size)
{
    if (size == 0) {
        // Cannot allocate zero size
        return NULL;
    }

    if (size > 2048) {
        printk("PANIC: %llu is larger than 2048. \n", size);
        return NULL;
    }

    int tier = get_tier(size);
    acquire_spinlock(&block_spinlock);

    page_header *p_page = partial_block_list[tier];
    // No empty list
    if (!p_page) {
        p_page = kalloc_page();
        if (!p_page) {
            printk("PANIC: cannot alloc page for tier %d, used pages: %lld, returning NULL\n",
                   tier, kalloc_page_cnt.count);
            return NULL;
        }

        setup_page(p_page, tier);
    }

    if (p_page->tier != tier) {
        printk("PANIC: tier mismatch, wanted %d, given %d\n", tier,
               p_page->tier);
    }

    void *addr = p_page->free_block;
    if (!addr) {
        printk("PANIC: full page in partial list\n");
    }
    p_page->free_block = *((char **)addr);

    p_page->filled_blocks++;
    if (!p_page->free_block) {
        if (p_page->prev) {
            p_page->prev->next = p_page->next;
        } else {
            partial_block_list[p_page->tier] = p_page->next;
        }

        if (p_page->next) {
            p_page->next->prev = p_page->prev;
        }

        p_page->prev = p_page->next = NULL;
    }

    release_spinlock(&block_spinlock);
    return addr;
}

u64 left_page_cnt()
{
    return total_page_cnt - kalloc_page_cnt.count;
}

void kfree(void *ptr)
{
    if (!ptr) {
        printk("Kfree NULL pointer\n");
        return;
    }

    acquire_spinlock(&block_spinlock);
    page_header *p_page = ALIGN_DOWN_PTR(ptr, PAGE_SIZE);

    if (!p_page->free_block) {
        int tier_ = p_page->tier;
        if (partial_block_list[tier_]) {
            partial_block_list[tier_]->prev = p_page;
        }

        p_page->next = partial_block_list[tier_];
        p_page->prev = NULL;
        partial_block_list[tier_] = p_page;
    }

    *((char **)ptr) = p_page->free_block;
    p_page->free_block = ptr;

    p_page->filled_blocks--;
    if (p_page->filled_blocks <= 0) {
        if (p_page->prev) {
            p_page->prev->next = p_page->next;
        } else {
            partial_block_list[p_page->tier] = p_page->next;
        }

        if (p_page->next) {
            p_page->next->prev = p_page->prev;
        }

        p_page->prev = p_page->next = NULL;
        kfree_page(p_page);
    }

    release_spinlock(&block_spinlock);
    return;
}

void *share_page(void *ptr)
{
    if (ptr == zero_page) {
        return ptr;
    }

    u32 page_index = ((char *)ptr - page_pool_start) / PAGE_SIZE;
    ASSERT(pages[page_index].ref.count > 0);
    increment_rc(&pages[page_index].ref);

    return ptr;
}

WARN_RESULT void *get_zero_page()
{
    if (!zero_page) {
        zero_page = kalloc_page();
        memset(zero_page, 0, PAGE_SIZE);
        u32 page_index = ((char *)zero_page - page_pool_start) / PAGE_SIZE;
        pages[page_index].ref.count = __INT_MAX__;
    }

    return zero_page;
}
