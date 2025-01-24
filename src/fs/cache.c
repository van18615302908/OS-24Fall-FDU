#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <common/rc.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block device and super block to use.
            Correspondingly, you should NEVER use global instance of
            them, e.g. `get_super_block`, `block_device`

    @see init_bcache
 */
static const SuperBlock *sblock;

/**
    @brief the reference to the underlying block device.
 */
static const BlockDevice *device;

/**
    @brief global lock for block cache.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory block.

    We use a linked list to manage all allocated cached blocks.

    You can implement your own data structure if you like better performance.

    @see Block
 */
static ListNode head;

static LogHeader header; // in-memory copy of log header block.

static RefCount num_cached_blocks;

// The block number from which the filesystem starts
extern u64 fs_start;

/**
    @brief a struct to maintain other logging states.
    
    You may wonder where we store some states, e.g.
    
    * how many atomic operations are running?
    * are we checkpointing?
    * how to notify `end_op` that a checkpoint is done?

    Put them here!

    @see cache_begin_op, cache_end_op, cache_sync
 */
struct {
    /* your fields here */
    SpinLock lock;
    Semaphore sem;
    bool committing;
    int num_ops;
} log;

// read the content from disk.
static INLINE void device_read(Block *block)
{
    device->read(fs_start + block->block_no, block->data);
}

// write the content back to disk.
static INLINE void device_write(Block *block)
{
    device->write(fs_start + block->block_no, block->data);
}

// read log header from disk.
static INLINE void read_header()
{
    device->read(fs_start + sblock->log_start, (u8 *)&header);
}

// write log header back to disk.
static INLINE void write_header()
{
    device->write(fs_start + sblock->log_start, (u8 *)&header);
}

// initialize a block struct.
static void init_block(Block *block) {
    // 初始化块号为 0。块号用于标识块在磁盘中的位置。
    // 在实际使用前，块号会被设置为正确的值。
    block->block_no = 0;

    init_list_node(&block->node);
    // 当线程获取某个块时，这个标志会被设置为 true。
    block->acquired = false;

    // 如果该块被修改或正在使用，它会被标记为 pinned = true，防止被驱逐。
    block->pinned = false;

    // 初始化块的睡眠锁。睡眠锁用于保护块的有效性和数据，确保在多个线程访问块时能够正确同步。
    init_sleeplock(&block->lock);
    // 当从磁盘加载内容后，这个标志会被设置为 true。
    block->valid = false;

    // 将块的数据缓冲区清零，这样可以确保在块被初始化时没有任何残留数据。
    // 块的大小是 `sizeof(block->data)`，即每个块的实际存储空间。
    memset(block->data, 0, sizeof(block->data));
}

// see `cache.h`.
static usize get_num_cached_blocks() {
    // TODO
    int num = 0;
    _for_in_list(node, &head){
        if(node == &head){
            continue;
        }
        num ++;
    }
    return num;
}   

static void cache_evict()
{
    for (ListNode *node = head.prev; node != &head; node = node->prev) {
        Block *blk = container_of(node, Block, node);

        if (blk->acquired || blk->pinned) {
            continue;  // 跳过已被获取或固定的块
        }

        _detach_from_list(&blk->node);
        decrement_rc(&num_cached_blocks);
        kfree(blk);

        if (num_cached_blocks.count < EVICTION_THRESHOLD) {
            break;  // 达到阈值后停止
        }
    }
}

Block *find_block_by_block_no(usize block_no)//根据块号查找块
{
    for (ListNode *node = head.next; node != &head; node = node->next) {
        Block *blk = container_of(node, Block, node);

        if (blk->block_no == block_no) {
            return blk;  // 找到后直接返回
        }
    }
    return NULL;  // 没找到返回 NULL
}

static Block *cache_acquire(usize block_no)
{
    acquire_spinlock(&lock);
    Block *blk = find_block_by_block_no(block_no);

    // 如果缓存块不存在，则从磁盘读取
    if (!blk) {
        if (get_num_cached_blocks() >= EVICTION_THRESHOLD) {
            cache_evict(); // 驱逐块以腾出空间
        }

        blk = (Block *)kalloc(sizeof(Block));
        init_block(blk);
        blk->block_no = block_no;

        release_spinlock(&lock);
        device_read(blk); // 读取磁盘数据到块
        acquire_spinlock(&lock);

        blk->valid = true;
        blk->acquired = true;

        increment_rc(&num_cached_blocks);
        _insert_into_list(&head, &blk->node);
    } else {
        blk->acquired = true;
    }

    if (!acquire_sleeplock(&blk->lock)) {
        release_spinlock(&lock);
        return NULL;
    }

    release_spinlock(&lock);
    return blk;
}

// see `cache.h`.
static void cache_release(Block *block)
{
    release_sleeplock(&block->lock);

    acquire_spinlock(&lock);
    block->acquired = false;
    _detach_from_list(&block->node);
    _insert_into_list(&head, &block->node);

    release_spinlock(&lock);
}

void commit_log()
{
    for (u64 i = 0; i < header.num_blocks; i++) {
        acquire_spinlock(&lock);
        Block *blk = find_block_by_block_no(header.block_no[i]);
        if (!blk) {
            PANIC();
        }
        Block write_blk = {
            .block_no = sblock->log_start + i + 1,
        };
        memcpy(write_blk.data, blk->data, BLOCK_SIZE);
        blk->pinned = false;
        release_spinlock(&lock);

        device_write(&write_blk); // 写入日志区域
    }

    write_header(); // 写入头部信息
}

void commit_data()
{
    if (header.num_blocks == 0) {
        return;
    }

    for (u64 i = 0; i < header.num_blocks; i++) {
        Block log_blk = {
            .block_no = sblock->log_start + i + 1,
        };
        device_read(&log_blk); // 从日志区域读取

        log_blk.block_no = header.block_no[i];
        device_write(&log_blk); // 写入实际存储区域
    }

    header.num_blocks = 0;
    write_header(); // 更新元数据并写入磁盘
}

// see `cache.h`.
void init_bcache(const SuperBlock *_sblock, const BlockDevice *_device)
{
    sblock = _sblock;
    device = _device;

    // TODO
    init_spinlock(&lock);
    init_spinlock(&log.lock);
    init_sem(&log.sem, 1);
    init_rc(&num_cached_blocks);
    init_list_node(&head);

    log.committing = false;
    log.num_ops = 0;

    read_header();
    commit_data();
}

// see `cache.h`.
static void cache_begin_op(OpContext *ctx)
{
    // TODO
    if (!ctx) {
        PANIC();
    }

    acquire_spinlock(&log.lock);
    while (log.committing ||
           header.num_blocks + (log.num_ops + 1) * OP_MAX_NUM_BLOCKS >
                   LOG_MAX_SIZE) {
        _lock_sem(&log.sem);
        release_spinlock(&log.lock);
        if (!_wait_sem(&log.sem, 1)) {
            return;
        }
        acquire_spinlock(&log.lock);
    }

    log.num_ops++;
    ctx->rm = OP_MAX_NUM_BLOCKS;
    release_spinlock(&log.lock);
}

// see `cache.h`.
static void cache_sync(OpContext *ctx, Block *block)
{
    if (!ctx) {
        device_write(block);
        return;
    }

    acquire_spinlock(&log.lock);

    if (block->pinned) {
        release_spinlock(&log.lock);
        return;
    }

    if (ctx->rm <= 0) {
        PANIC();
    }
    ctx->rm--;

    block->pinned = true; // 标记块为脏
    header.block_no[header.num_blocks++] = block->block_no;

    release_spinlock(&log.lock);
}

// see `cache.h`.
static void cache_end_op(OpContext *ctx)
{
    // TODO
    if (!ctx) {
        PANIC();
    }

    acquire_spinlock(&log.lock);
    if (log.committing) {
        PANIC();
    }

    log.num_ops--;
    if (log.num_ops > 0) {
        post_all_sem(&log.sem);
        release_spinlock(&log.lock);
        return;
    }

    log.committing = true;
    release_spinlock(&log.lock);

    commit_log();

    commit_data();
    acquire_spinlock(&log.lock);
    log.committing = false;
    post_all_sem(&log.sem);
    release_spinlock(&log.lock);
}


// see `cache.h`.
static usize cache_alloc(OpContext *ctx)
{
    if (!ctx) {
        PANIC();
    }

    for (usize i = 0; i < sblock->num_blocks; i += BIT_PER_BLOCK) {
        Block *bitmap_block = cache_acquire(sblock->bitmap_start + i / BIT_PER_BLOCK);

        for (usize j = 0; j < BIT_PER_BLOCK && i + j < sblock->num_blocks; j++) {
            u8 probe = 1u << (j % 8u);
            if (!(bitmap_block->data[j / 8] & probe)) { // 如果块是空闲的
                bitmap_block->data[j / 8] |= probe;
                cache_sync(ctx, bitmap_block);
                cache_release(bitmap_block);

                Block allocated_blk = {
                    .block_no = i + j,
                };
                memset(allocated_blk.data, 0, BLOCK_SIZE);
                device_write(&allocated_blk);
                return allocated_blk.block_no;
            }
        }

        cache_release(bitmap_block);
    }

    printk("PANIC: No free block remaining.\n");
    PANIC();
}

// see `cache.h`.
static void cache_free(OpContext *ctx, usize block_no)
{
    const usize bitmap_block_no =
            sblock->bitmap_start + block_no / BIT_PER_BLOCK;
    Block *bitmap_block = cache_acquire(bitmap_block_no);

    usize in_block_index = block_no % BIT_PER_BLOCK;
    u8 probe = 1u << (in_block_index % 8u);
    bitmap_block->data[in_block_index / 8] &= ~probe;
    cache_sync(ctx, bitmap_block);
    cache_release(bitmap_block);
}

BlockCache bcache = {
    .get_num_cached_blocks = get_num_cached_blocks,
    .acquire = cache_acquire,
    .release = cache_release,
    .begin_op = cache_begin_op,
    .sync = cache_sync,
    .end_op = cache_end_op,
    .alloc = cache_alloc,
    .free = cache_free,
};