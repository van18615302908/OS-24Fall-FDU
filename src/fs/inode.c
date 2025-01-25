#include <common/string.h>
#include <fs/inode.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <fs/cache.h>
#include <kernel/sched.h>
#include <kernel/console.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block cache and super block to use.
            Correspondingly, you should NEVER use global instance of
            them.

    @see init_inodes
 */
static const SuperBlock *sblock;

/**
    @brief the reference to the underlying block cache.
 */
static const BlockCache *cache;

/**
    @brief global lock for inode layer.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, ref counts, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory inodes.

    We use a linked list to manage all allocated inodes.

    You can implement your own data structure if you want better performance.

    @see Inode
 */
static ListNode head;



// initialize inode tree.
void init_inodes(const SuperBlock *_sblock, const BlockCache *_cache)
{
    init_spinlock(&lock);
    init_list_node(&head);
    sblock = _sblock;
    cache = _cache;

    if (ROOT_INODE_NO < sblock->num_inodes)
        inodes.root = inodes.get(ROOT_INODE_NO);
    else
        printk("(warn) init_inodes: no root inode.\n");
}

// initialize in-memory inode.
static void init_inode(Inode *inode)
{
    init_sleeplock(&inode->lock);
    init_rc(&inode->rc);
    init_list_node(&inode->node);
    inode->inode_no = 0;
    inode->valid = false;
}

// see `inode.h`.
static usize inode_alloc(OpContext *ctx, InodeType type)
{
    ASSERT(type != INODE_INVALID);

    usize block_no = TO_BLOCK_NO(ROOT_INODE_NO); // 起始块号
    Block *inode_block = cache->acquire(block_no);

    for (usize i = 1; i < sblock->num_inodes; i++) {
        // 如果是新的块，则加载对应的块
        if (i % INODE_PER_BLOCK == 0) {
            cache->release(inode_block);
            block_no = TO_BLOCK_NO(i);
            inode_block = cache->acquire(block_no);
        }

        // 获取当前块中 inode 的 entry
        InodeEntry *entry = GET_ENTRY(inode_block, i);

        // 如果找到空闲 inode
        if (entry->type == 0) {
            memset(entry, 0, sizeof(InodeEntry)); // 初始化为 0
            entry->type = type; // 设置类型，标记为已用
            cache->sync(ctx, inode_block); // 同步缓存
            cache->release(inode_block); // 释放块
            return i;
        }
    }

    // 如果未找到空闲 inode
    cache->release(inode_block);
    return 0;
}

// see `inode.h`.
static void inode_lock(Inode *inode)
{
    ASSERT(inode->rc.count > 0);
    if (!inode->valid) {
        Block *inode_block = cache->acquire(TO_BLOCK_NO(inode->inode_no));
        InodeEntry *inodes = (InodeEntry *)inode_block->data;

        memcpy(&inode->entry, &inodes[inode->inode_no % INODE_PER_BLOCK],
               sizeof(InodeEntry));
        cache->release(inode_block);
        inode->valid = true;
    }
}

// see `inode.h`.
static void inode_unlock(Inode *inode)
{
    ASSERT(inode->rc.count > 0);
    release_sleeplock(&inode->lock);
}

// see `inode.h`.
static void inode_sync(OpContext *ctx, Inode *inode, bool do_write)
{
    // TODO
    Block *inode_block = cache->acquire(TO_BLOCK_NO(inode->inode_no));
    InodeEntry *inodes = (InodeEntry *)inode_block->data;
    usize inode_index = inode->inode_no % INODE_PER_BLOCK;

    if (do_write) {
        // Cannot write invalid data
        ASSERT(inode->valid);

        // Write data to disk
        memcpy(&inodes[inode_index], &inode->entry, sizeof(InodeEntry));
        cache->sync(ctx, inode_block);
        cache->release(inode_block);
    } else if (!inode->valid) {
        // Read data from disk if not present
        memcpy(&inode->entry, &inodes[inode_index], sizeof(InodeEntry));
        cache->release(inode_block);
        inode->valid = true;
    } else {
        // Do nothing if data is present and not `do_write`
        cache->release(inode_block);
    }
}



// see `inode.h`.
static Inode *inode_get(usize inode_no)
{
    ASSERT(inode_no > 0);
    ASSERT(inode_no < sblock->num_inodes);
    acquire_spinlock(&lock);
    // TODO
    Inode *inode = NULL;

    ListNode *node = head.next;

    while (node != &head) {
        Inode *current_inode = container_of(node, Inode, node);
        if (current_inode->inode_no == inode_no) {
            inode = current_inode;
            break;
        }

        node = node->next;
    }


    if (!inode) {
        inode = (Inode *)kalloc(sizeof(Inode));
        init_inode(inode);
        inode->inode_no = inode_no;
        _insert_into_list(&head, &inode->node);
    }

    increment_rc(&inode->rc);
    release_spinlock(&lock);
    return inode;
}

/* 清理直接块 */
static void clear_direct_blocks(OpContext *ctx, u32 *direct_addrs)
{
    for (u64 i = 0; i < INODE_NUM_DIRECT; i++) {
        if (direct_addrs[i]) {
            cache->free(ctx, direct_addrs[i]);
            direct_addrs[i] = 0;
        }
    }
}

/* 清理间接块 */
static void clear_indirect_block(OpContext *ctx, Inode *inode)
{
    if (inode->entry.indirect) {
        Block *indirect_blk = cache->acquire(inode->entry.indirect);
        u32 *indirect_addrs = GET_ADDRS(indirect_blk);

        for (u64 i = 0; i < INODE_NUM_INDIRECT; i++) {
            if (indirect_addrs[i]) {
                cache->free(ctx, indirect_addrs[i]);
                indirect_addrs[i] = 0;
            }
        }

        cache->release(indirect_blk);
        cache->free(ctx, inode->entry.indirect);
        inode->entry.indirect = 0;
    }
}

// see `inode.h`.
static void inode_clear(OpContext *ctx, Inode *inode)
{
    clear_direct_blocks(ctx, inode->entry.addrs); // 清理直接块
    clear_indirect_block(ctx, inode);            // 清理间接块

    // 清空 inode 的字节数并同步到磁盘
    inode->entry.num_bytes = 0;
    inode_sync(ctx, inode, true);
}


// see `inode.h`.
static Inode *inode_share(Inode *inode)
{
    // TODO
    ASSERT(inode != NULL);
    increment_rc(&inode->rc);
    return inode;
}

/* 处理没有链接的 inode */
static void handle_inode_no_links(OpContext *ctx, Inode *inode)
{
    if (inode->entry.num_links == 0) {
        inode_clear(ctx, inode);           
        inode->entry.type = INODE_INVALID; 
        inode_sync(ctx, inode, true);      
        inode->valid = false;              
    }
}

/* 如果 inode 不再被引用，释放其内存 */
static void free_inode_if_unused(Inode *inode)
{
    if (inode->rc.count == 0) {
        _detach_from_list(&inode->node); 
        kfree(inode);                   
    }
}

// see `inode.h`.
static void inode_put(OpContext *ctx, Inode *inode)
{
    if (!inode->valid) {
        inode_sync(ctx, inode, false); // 同步 inode 数据到磁盘
    }

    acquire_spinlock(&lock);
    decrement_rc(&inode->rc); // 减少引用计数

    if (inode->rc.count == 0) {
        handle_inode_no_links(ctx, inode); // 处理无链接的 inode
        free_inode_if_unused(inode);       
    }

    release_spinlock(&lock);
}

/**
    @brief get which block is the offset of the inode in.

    e.g. `inode_map(ctx, my_inode, 1234, &modified)` will return the block_no
    of the block that contains the 1234th byte of the file
    represented by `my_inode`.

    If a block has not been allocated for that byte, `inode_map` will
    allocate a new block and update `my_inode`, at which time, `modified`
    will be set to true.

    HOWEVER, if `ctx == NULL`, `inode_map` will NOT try to allocate any new block,
    and when it finds that the block has not been allocated, it will return 0.
    
    @param[out] modified true if some new block is allocated and `inode`
    has been changed.

    @return usize the block number of that block, or 0 if `ctx == NULL` and
    the required block has not been allocated.

    @note the caller must hold the lock of `inode`.
 */
/* 判断是否在直接块范围内 */
static INLINE bool is_within_direct_blocks(usize offset)
{
    return offset < BLOCK_SIZE * INODE_NUM_DIRECT;
}

/* 判断是否在间接块范围内 */
static INLINE bool is_within_indirect_blocks(usize offset)
{
    return offset < BLOCK_SIZE * INODE_NUM_INDIRECT;
}

/* 处理直接块的映射 */
static usize handle_direct_block_mapping(OpContext *ctx, Inode *inode, usize offset, bool *modified)
{
    u32 block_index = offset / BLOCK_SIZE;

    // 如果直接块还未分配，则进行分配
    if (inode->entry.addrs[block_index] == 0) {
        if (!ctx) {
            printk(" inode_map: no ctx,\n");
            return 0;
        }

        inode->entry.addrs[block_index] = cache->alloc(ctx);
        *modified = true;
    }

    return inode->entry.addrs[block_index];
}

/* 处理间接块的映射 */
static usize handle_indirect_block_mapping(OpContext *ctx, Inode *inode, usize offset, bool *modified)
{
    // 如果间接块指针尚未分配，则分配间接块
    if (inode->entry.indirect == 0) {
        if (!ctx) {
            printk("inode_map no ctx cannot create indirect table block\n");
            return 0;
        }

        inode->entry.indirect = cache->alloc(ctx);
        *modified = true;
    }

    // 获取间接块
    Block *indirect_blk = cache->acquire(inode->entry.indirect);
    u32 block_index = offset / BLOCK_SIZE;

    u32 *indirect_addrs = GET_ADDRS(indirect_blk);

    // 如果间接块中的地址尚未分配，则分配数据块
    if (indirect_addrs[block_index] == 0) {
        if (!ctx) {
            printk("inode_map no ctx\n");
            cache->release(indirect_blk);
            return 0;
        }

        indirect_addrs[block_index] = cache->alloc(ctx);
        cache->sync(ctx, indirect_blk);
        *modified = true;
    }

    cache->release(indirect_blk);
    return indirect_addrs[block_index];
}

// see `inode.h`.
static usize inode_map(OpContext *ctx, Inode *inode, usize offset, bool *modified)
{
    *modified = false;

    // 处理直接块映射
    if (is_within_direct_blocks(offset)) {
        return handle_direct_block_mapping(ctx, inode, offset, modified);
    }

    // 处理间接块映射
    offset -= BLOCK_SIZE * INODE_NUM_DIRECT;
    if (is_within_indirect_blocks(offset)) {
        return handle_indirect_block_mapping(ctx, inode, offset, modified);
    }

    // 文件太大，超出支持范围
    printk("inode_map too large.\n");
    return 0;
}

// see `inode.h`.
static usize inode_read(Inode *inode, u8 *dest, usize offset, usize count) {
    ASSERT(inode != NULL);
    InodeEntry *entry = &inode->entry;

    // 处理设备文件的读取
    if (entry->type == INODE_DEVICE) {
        return console_read(inode, (char *)dest, count);  
    }

    // 调整读取范围，避免越界
    if (offset >= entry->num_bytes) return 0;  // 如果偏移量超出文件大小，直接返回
    usize end = MIN(offset + count, entry->num_bytes);  // 计算实际的读取终点
    usize current_position = offset;

    while (current_position < end) {
        bool modified;
        u32 data_blk_no = inode_map(NULL, inode, current_position, &modified);
        ASSERT(!modified);

        // 如果找不到数据块，终止读取
        if (data_blk_no == 0) {
            printk("cannot read data block, aborting. \n");
            break;
        }

        // 读取数据块内容
        Block *data_blk = cache->acquire(data_blk_no);
        u32 pos_in_block = current_position % BLOCK_SIZE;
        u32 read_count = MIN(BLOCK_SIZE - pos_in_block, end - current_position);
        memcpy(dest, &data_blk->data[pos_in_block], read_count);

        cache->release(data_blk);  // 释放缓存块
        current_position += read_count;
        dest += read_count;
    }

    return current_position - offset;  // 返回实际读取的字节数
}

// see `inode.h`.
static usize inode_write(OpContext *ctx, Inode *inode, u8 *src, usize offset,
                         usize count)
{
    ASSERT(inode != NULL);
    ASSERT(ctx != NULL);
    ASSERT(count <= OP_MAX_NUM_BLOCKS * BLOCK_SIZE);
    InodeEntry *entry = &inode->entry;

    if (entry->type == INODE_DEVICE) {
        return console_write(inode, (char *)src, count);
    }

    ASSERT(offset <= entry->num_bytes);
    usize end = (offset + count);
    ASSERT(end <= INODE_MAX_BYTES);
    ASSERT(offset <= end);

    bool need_sync = false;
    usize position = offset;
    while (position < end) {
        bool modi;
        u32 data_no = inode_map(ctx, inode, position, &modi);
        need_sync = need_sync || modi;

        if (data_no == 0) {
            printk("cannot write data block, aborting. \n");
            return position - offset;
        }

        Block *data_blk = cache->acquire(data_no);
        u32 pos_in_block = position % BLOCK_SIZE;
        u32 write_count = MIN(BLOCK_SIZE - pos_in_block, end - position);
        memcpy(&data_blk->data[pos_in_block], src, write_count);


        cache->sync(ctx, data_blk);
        cache->release(data_blk);
        position += write_count;
        src += write_count;
    }

    if (position > inode->entry.num_bytes) {
        need_sync = true;
        inode->entry.num_bytes = position;
    }

    if (need_sync) inode_sync(ctx, inode, true);

    return position - offset;
}

// see `inode.h`.
static usize inode_lookup(Inode *inode, const char *name, usize *index)
{
    InodeEntry *entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    u32 len_name = strlen(name);//获取文件名长度
    DirEntry dir_entry;
    for (u32 i = 0; i < inode->entry.num_bytes; i += sizeof(DirEntry)) {
        usize size_to_read =inode_read(inode, (u8 *)&dir_entry, i, sizeof(DirEntry));
        ASSERT(size_to_read == sizeof(DirEntry));

        if (dir_entry.inode_no == 0) {
            continue;
        }

        u32 len_entry = strlen(dir_entry.name);
        if (len_entry != len_name) {
            continue;
        }

        if (!strncmp(name, dir_entry.name, len_entry)) {
            if (index) *index = i / sizeof(DirEntry);
            return dir_entry.inode_no;
        }
    }

    // Not found
    return 0;
}

// see `inode.h`.
static isize inode_insert(OpContext *ctx, Inode *inode, const char *name,
                          usize inode_no)
{
    InodeEntry *entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    usize dir_index;
    u32 block_no = inode_lookup(inode, name, &dir_index);

    // Already exists
    if (block_no != 0) {
        return -1;
    }

    DirEntry dir_entry;
    isize entry_offset = 0;
    for (; entry_offset < inode->entry.num_bytes;
         entry_offset += sizeof(DirEntry)) {
        usize read_size = inode_read(inode, (u8 *)&dir_entry, entry_offset,
                                     sizeof(DirEntry));
        ASSERT(read_size == sizeof(DirEntry));

        // Empty item
        if (dir_entry.inode_no == 0) {
            break;
        }
    }

    dir_entry.inode_no = inode_no;
    ASSERT(strlen(name) <= FILE_NAME_MAX_LENGTH - 1);
    memcpy(dir_entry.name, name, strlen(name) + 1);

    // Test if write succeeds
    if (inode_write(ctx, inode, (u8 *)&dir_entry, entry_offset,
                    sizeof(DirEntry)) != sizeof(DirEntry)) {
        printk("inode insertion failed due to write fault\n");
        return -1;
    }
    return entry_offset / sizeof(DirEntry);
}

// see `inode.h`.
static void inode_remove(OpContext *ctx, Inode *inode, usize index)
{
    InodeEntry *entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    DirEntry dir_entry;
    usize read_size = inode_read(inode, (u8 *)&dir_entry,
                                 index * sizeof(DirEntry), sizeof(DirEntry));
    ASSERT(read_size == sizeof(DirEntry));

    if (dir_entry.inode_no != 0) {
        // TODO: Remove the inode of `inode_no` if applicable, but this is not necessary in this lab
        dir_entry.inode_no = 0;
        inode_write(ctx, inode, (u8 *)&dir_entry, index * sizeof(DirEntry),
                    sizeof(DirEntry));
    }
}

InodeTree inodes = {
    .alloc = inode_alloc,
    .lock = inode_lock,
    .unlock = inode_unlock,
    .sync = inode_sync,
    .get = inode_get,
    .clear = inode_clear,
    .share = inode_share,
    .put = inode_put,
    .read = inode_read,
    .write = inode_write,
    .lookup = inode_lookup,
    .insert = inode_insert,
    .remove = inode_remove,
};

/**
    @brief read the next path element from `path` into `name`.
    
    @param[out] name next path element.

    @return const char* a pointer offseted in `path`, without leading `/`. If no
    name to remove, return NULL.

    @example 
    skipelem("a/bb/c", name) = "bb/c", setting name = "a",
    skipelem("///a//bb", name) = "bb", setting name = "a",
    skipelem("a", name) = "", setting name = "a",
    skipelem("", name) = skipelem("////", name) = NULL, not setting name.
 */
static const char *skipelem(const char *path, char *name)
{
    const char *s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= FILE_NAME_MAX_LENGTH)
        memmove(name, s, FILE_NAME_MAX_LENGTH);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

/**
    @brief look up and return the inode for `path`.

    If `nameiparent`, return the inode for the parent and copy the final
    path element into `name`.
    
    @param path a relative or absolute path. If `path` is relative, it is
    relative to the current working directory of the process.

    @param[out] name the final path element if `nameiparent` is true.

    @return Inode* the inode for `path` (or its parent if `nameiparent` is true), 
    or NULL if such inode does not exist.

    @example
    namex("/a/b", false, name) = inode of b,
    namex("/a/b", true, name) = inode of a, setting name = "b",
    namex("/", true, name) = NULL (because "/" has no parent!)
 */

// 辅助函数：处理 inode 错误
static void handle_inode_error(OpContext *ctx, Inode *inode, const char *message) {
    // printk(" %s\n", message);
    inode_unlock(inode);
    inode_put(ctx, inode);
}

// 辅助函数：获取路径对应的 inode
static Inode *namex(const char *path, bool nameiparent, char *name, OpContext *ctx) {
    ASSERT(path != NULL && name != NULL && ctx != NULL);

    // 获取起始 Inode
    Inode *current_inode = (path[0] == '/') ? inode_share(inodes.root) : inode_share(thisproc()->cwd);
    if (!current_inode) {
        printk("Failed to get starting inode.\n");
        return NULL;
    }

    // 遍历路径
    while ((path = skipelem(path, name)) != NULL) {
        inode_lock(current_inode);

        // 检查当前 inode 是否为目录
        if (current_inode->entry.type != INODE_DIRECTORY) {
            handle_inode_error(ctx, current_inode, "Non-directory inode encountered.");
            return NULL;
        }

        // 如果路径结束且需要返回父目录，直接返回
        if (path[0] == '\0' && nameiparent) {
            inode_unlock(current_inode);
            return current_inode;
        }

        // 查找下一级目录
        usize next_inode_no = inode_lookup(current_inode, name, NULL);
        if (next_inode_no == 0) {
            handle_inode_error(ctx, current_inode, "Failed to find next directory.");
            return NULL;
        }

        // 获取下一级 inode
        Inode *next_inode = inode_get(next_inode_no);
        ASSERT(next_inode != NULL);

        // 释放当前 inode
        inode_unlock(current_inode);
        inode_put(ctx, current_inode);

        // 切换到下一级 inode
        current_inode = next_inode;
    }

    // 如果需要父目录但未找到合适的，释放资源
    if (nameiparent) {
        inode_put(ctx, current_inode);
        return NULL;
    }

    return current_inode;
}



Inode *namei(const char *path, OpContext *ctx)
{
    char name[FILE_NAME_MAX_LENGTH];
    return namex(path, false, name, ctx);
}

Inode *nameiparent(const char *path, char *name, OpContext *ctx)
{
    return namex(path, true, name, ctx);
}

/**
    @brief get the stat information of `ip` into `st`.
    
    @note the caller must hold the lock of `ip`.
 */
void stati(Inode *ip, struct stat *st)
{
    st->st_dev = 1;
    st->st_ino = ip->inode_no;
    st->st_nlink = ip->entry.num_links;
    st->st_size = ip->entry.num_bytes;
    switch (ip->entry.type) {
    case INODE_REGULAR:
        st->st_mode = S_IFREG;
        break;
    case INODE_DIRECTORY:
        st->st_mode = S_IFDIR;
        break;
    case INODE_DEVICE:
        st->st_mode = 0;
        break;
    default:
        PANIC();
    }
}