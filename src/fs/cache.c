#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
int debug_cache = 0;
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
    SpinLock lock;//保护log的读写
    Semaphore begin;//begin_op
    Semaphore end;//end_op
    u32 outstanding;//未完成的log块数
    bool committing;//是否正在commit
} log;

SpinLock head_lock;

// read the content from disk.
static INLINE void device_read(Block *block) {
    device->read(block->block_no, block->data);
}

// write the content back to disk.
static INLINE void device_write(Block *block) {
    device->write(block->block_no, block->data);
}

// read log header from disk.
static INLINE void read_header() {
    device->read(sblock->log_start, (u8 *)&header);
}

// write log header back to disk.
static INLINE void write_header() {
    device->write(sblock->log_start, (u8 *)&header);
}



// initialize a block struct.
static void init_block(Block *block) {
    // if(debug_cache){
    //     printk("init_block\n");
    // }
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

// see `cache.h`.
static Block *cache_acquire(usize block_no) {
    // TODO
    if(debug_cache){
        printk("cache_acquire 编号：%llu\n", block_no);
    }
    acquire_spinlock(&lock);

    _for_in_list(p, &head){
        //遍历块缓存链表
        if(p == &head) continue;
        Block* b = container_of(p, Block, node);
        while(b->block_no == block_no){
            if(b->acquired) {
                release_spinlock(&lock);
                
                unalertable_wait_sem(&b->lock);

                acquire_spinlock(&lock);
                if (get_sem(&b->lock)) {
                    b->acquired = true;
                    break;
                }
            }
            if(!b->acquired){
                //如果块没有被获取，那么获取它
                get_sem(&b->lock);//get_sem只有lock为正数才会减少信号量
                b->acquired = true;
            }
            acquire_spinlock(&head_lock);
            _detach_from_list(p);
            _insert_into_list(&head, p);
            // printk("_insert_into_list%lld\n",b->block_no);
            release_spinlock(&head_lock);

            release_spinlock(&lock);
            return b;
        }
    }
    // 如果缓存块数量超出阈值，执行块驱逐操作
    //块驱逐之后，会分配一个新的块，并初始化它
    usize cnum = get_num_cached_blocks();
   
    if(cnum >= EVICTION_THRESHOLD){
        ListNode* p = head.prev;//找到链表的尾部

        while(p != &head && cnum >= EVICTION_THRESHOLD){
            Block* b = container_of(p, Block, node);
            if(!b->pinned && !b->acquired ){
                //如果该块未被固定且未被获取，则可以安全地驱逐
                if(debug_cache){
                    printk("驱逐块 编号：%llu\n", b->block_no);
                }
                if(p->prev ==p){
                    printk("自环！驱逐块 编号：%llu\n\n", b->block_no); 
                    return NULL;
                }
                _detach_from_list(p);
                kfree(b);
                cnum = get_num_cached_blocks();
                p = head.prev;
            }else{
                //否则继续遍历
                p = p->prev;
            }
        }
    }
    
     // 分配一个新的块，并初始化它
    Block* block = kalloc(sizeof(Block));
    if(debug_cache){
        printk("分配块 编号：%llu\n", block_no);
    }
    init_block(block);
    block->block_no = block_no;
    block->valid = true;
    block->acquired = true;

    acquire_spinlock(&head_lock);
    _detach_from_list(&block->node);
    _insert_into_list(&head, &block->node);
    release_spinlock(&head_lock);

    // unalertable_wait_sem(&block->lock);

    device_read(block);
    release_spinlock(&lock);
    return block;
}


// see `cache.h`.
static void cache_release(Block *block) {
    if(debug_cache){
        printk("cache_release 编号：%llu on CPU:%lld\n", block->block_no, cpuid());
    }
    // TODO
    // ASSERT(block->acquired);
    acquire_spinlock(&lock);
    block->acquired = false;
    post_sem(&block->lock);
    release_spinlock(&lock);
}



static void log_wb(){
    if(debug_cache){
        printk("log_wb\n");
    }
    for(usize i = 0; i < header.num_blocks; i++){
        Block* logb = cache_acquire(sblock->log_start + i + 1);
        Block* sdb = cache_acquire(header.block_no[i]);
        memmove(sdb->data, logb->data, BLOCK_SIZE);
        device_write(sdb);
        sdb->pinned = false;
        cache_release(logb);
        cache_release(sdb);
    }
    header.num_blocks = 0;
    write_header();
}

// see `cache.h`.
void init_bcache(const SuperBlock *_sblock, const BlockDevice *_device) {
    if(debug_cache){
        printk("init_bcache\n");
    }
    sblock = _sblock;
    device = _device;

    // TODO
    //初始化块缓存
    init_spinlock(&lock);
    init_spinlock(&head_lock);
    init_list_node(&head);
    //初始化日志
    init_spinlock(&log.lock);
    init_sem(&log.begin, 0);
    init_sem(&log.end, 0);
    log.outstanding = 0;
    log.committing = false;
    //读取日志头
    read_header();

    log_wb();

}

// see `cache.h`.
static void cache_begin_op(OpContext *ctx) {
    if(debug_cache){
        printk("cache_begin_op\n");
    }
    // TODO
    acquire_spinlock(&log.lock);
    //更新日志状态
    ctx->rm = OP_MAX_NUM_BLOCKS;
    log.outstanding++;
    release_spinlock(&log.lock);
}

// see `cache.h`.
static void cache_sync(OpContext *ctx, Block *block) {
    // TODO
    //将缓存中的块同步到日志系统或直接写入磁盘
    if(ctx){
        usize i;
        acquire_spinlock(&log.lock);
        //查找块号是否已经在日志中
        for(i = 0; i < header.num_blocks; i++){
            // 如果找到了匹配的块号，说明该块已经在日志中，无需重复添加，退出循环。
            if(header.block_no[i] == block->block_no)
                break;
        }
        //将当前块号添加到日志头中，更新日志头的块号数组。
        header.block_no[i] = block->block_no;
        //如果块未被修改，不需要写入日志
        if(i == header.num_blocks){
            if(ctx->rm == 0)
                PANIC();

            // 减少上下文中的剩余块数，表示我们占用了一个块号记录空间。
            ctx->rm--;
            // 增加日志头中的块数量，表示我们记录了一个新的块号。
            header.num_blocks++;
            // 将块标记为 "pinned"，表示该块不能被缓存系统驱逐，直到日志提交完成。
            block->pinned = true;
        }
        release_spinlock(&log.lock);
    }else{
        //如果没有提供上下文 ctx，说明这不是一个事务操作，直接将块写入磁盘。
        device_write(block);
    }    
}

// see `cache.h`.
// static void cache_end_op(OpContext* ctx) {
//     if(debug_cache){
//         printk("cache_end_op\n");
//     }
//     acquire_spinlock(&log.lock);

//     // 释放当前操作占用的日志空间
//     ctx->rm = 0;  // 重置上下文中剩余的可用操作数

//     log.outstanding--;
//     if(log.outstanding > 0){
//         // 如果还有其他未完成的事务，允许其他线程继续操作日志
//         // post_all_sem(&log.begin);

//         // 等待日志提交完成
//         _lock_sem(&log.end);
//         release_spinlock(&log.lock);
//         ASSERT(_wait_sem(&log.end, false));
//         return;
        
//     }
//     // 如果没有其他未完成的日志操作，准备提交日志
//     if(log.outstanding == 0){
//         log.committing = true;


//         // 将日志中的块数据写入磁盘
//         for(usize i = 0; i < header.num_blocks; i++){
//             // 获取日志块和实际数据块
//             Block* logb = cache_acquire(sblock->log_start + i + 1);
//             Block* sdb = cache_acquire(header.block_no[i]);

//             // 将实际数据块的内容复制到日志块中
//             memmove(logb->data, sdb->data, BLOCK_SIZE);

//             device_write(logb);

//             // 释放日志块和数据块
//             cache_release(logb);
//             cache_release(sdb);
//         }

//         // 将日志头写入磁盘，完成日志记录
//         write_header();


//         // 写入所有日志，保证日志块已同步到磁盘
//         log_wb();

//         log.committing = false;
//         post_all_sem(&log.end);

//         // 释放自旋锁
//         release_spinlock(&log.lock);
//     }
// }


static void cache_end_op(OpContext *ctx) {
    (void)ctx;
    acquire_spinlock(&log.lock);
    log.outstanding--;
    // If there are other contributors to the log, we wait for them to complete
    if (log.outstanding > 0) {
        // Note on Potential Concurrency Issues
        // Here we DO NOT use unalertable_wait_sem because it is possible that
        // the contributor A here releases log.lock and contributor B immediately
        // grabs the lock and triggers `post_all_sem(&log.work_done);` below. 
        // In this case, the contributor A will never wake up because contributor A
        // triggers wait operation after contributor B triggers post_all_sem operation.
        _lock_sem(&(log.end));
        release_spinlock(&log.lock);
        if (!_wait_sem(&(log.end), false)) {
            PANIC();
        };
        return;
    }
    // After all the contributors call end_op, we write the cache back to the log,
    // sync the header in memory with the header in disk and finally write the checkpoint
    // to the disk.
    if (log.outstanding == 0) {
        write_log();
        write_header();
        spawn_ckpt();
        post_all_sem(&log.end);
    }
    release_spinlock(&log.lock);
}













// see `cache.h`.
static usize cache_alloc(OpContext *ctx) {
    if(debug_cache){
        printk("cache_alloc\n");
    }
    // 计算位图块的数量
    usize num_bitmap_blocks = (sblock->num_data_blocks + BIT_PER_BLOCK - 1) / BIT_PER_BLOCK;
    //向上取整，以确保能为所有数据块分配足够的位图块

    // 遍历位图块
    for(u32 i = 0; i < num_bitmap_blocks; i++){
        Block* b = cache_acquire(sblock->bitmap_start + i);
        BitmapCell* bm = (BitmapCell*)b->data;

        // 遍历位图块中的每一位
        for(u32 j = 0; j < BIT_PER_BLOCK; j++){
            // 检查是否超出数据块范围
            if(i * BIT_PER_BLOCK + j >= sblock->num_blocks){
                cache_release(b);
                PANIC();
            }

            // 如果找到空闲位
            if(!bitmap_get(bm, j)){
                // 设置位图中的对应位
                bitmap_set(bm, j);

                // 同步位图块到日志或磁盘
                cache_sync(ctx, b);

                // 释放位图块
                cache_release(b);

                // 获取新分配的块，并将其初始化为0
                Block* new = cache_acquire(i * BIT_PER_BLOCK + j);
                memset(new->data, 0, BLOCK_SIZE);

                // 同步新块到日志或磁盘
                cache_sync(ctx, new);
                cache_release(new);

                // 返回新分配的块号
                return i * BIT_PER_BLOCK + j;
            }
        }

        // 释放当前位图块
        cache_release(b);
    }

    // 如果无法分配块，则触发 PANIC
    PANIC();
}

// see `cache.h`.
static void cache_free(OpContext *ctx, usize block_no) {
    if(debug_cache){
        printk("cache_free\n");
    }
    // TODO
    Block* b = cache_acquire(sblock->bitmap_start + block_no / BIT_PER_BLOCK);
    BitmapCell* bm = (BitmapCell*)b->data;
    bitmap_clear(bm, block_no % BIT_PER_BLOCK);
    cache_sync(ctx, b);
    cache_release(b);
}


void write_log() {
    // Walk through every block in the log and write the changes back
    // to the log. Note that we don't just sequentially write the `blocks`
    // back because the sequence of the logged blocks is specified in
    // `block_no` of the log header.
    for (usize i = 0; i < header.num_blocks; i++) {
        Block* b = cache_acquire(header.block_no[i]);
        device->write(sblock->log_start + i + 1, b->data);
        b->pinned = false;
        cache_release(b);
    }
}

void spawn_ckpt() {
    // Sync the header.
    read_header();

    // The transfer block which holds the block read from log.
    Block transfer_b;
    init_block(&transfer_b);

    // Read the log into the transfer block.
    // 
    // Note that the block number of the block for logging and
    // the exact block number of the data are different.
    // In order to read the log, we need to figure out the block
    // number based on the number of log_start.
    // The exact block number of the logged blocks are stored
    // in the header of the log area.
    for(usize i = 0; i < header.num_blocks; i++) {
        transfer_b.block_no = sblock->log_start + 1 + i;
        device_read(&transfer_b);
        transfer_b.block_no = header.block_no[i];
        device_write(&transfer_b);
    }

    // Empty the log section.
    header.num_blocks = 0;
    write_header();
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