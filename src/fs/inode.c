#include <common/string.h>
#include <fs/inode.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/console.h>
#include <kernel/sched.h>
#include <sys/stat.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block cache and super block to use.
            Correspondingly, you should NEVER use global instance of
            them.

    @see init_inodes
 */
static const SuperBlock* sblock;

/**
    @brief the reference to the underlying block cache.
 */
static const BlockCache* cache;

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


// return which block `inode_no` lives on.
static INLINE usize to_block_no(usize inode_no) {
    return sblock->inode_start + (inode_no / (INODE_PER_BLOCK));
}

// return the pointer to on-disk inode.
static INLINE InodeEntry* get_entry(Block* block, usize inode_no) {
    return ((InodeEntry*)block->data) + (inode_no % INODE_PER_BLOCK);
}

// return address array in indirect block.
static INLINE u32* get_addrs(Block* block) {
    return ((IndirectBlock*)block->data)->addrs;
}

// initialize inode tree.
void init_inodes(const SuperBlock* _sblock, const BlockCache* _cache) {
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
static void init_inode(Inode* inode) {
    init_sleeplock(&inode->lock);
    init_rc(&inode->rc);
    init_list_node(&inode->node);
    inode->inode_no = 0;
    inode->valid = false;
}

// see `inode.h`.
static usize inode_alloc(OpContext* ctx, InodeType type) {
    ASSERT(type != INODE_INVALID);

    // TODO
    for(u32 i = 1; i < sblock->num_inodes; i++){
        //查找一个空闲的inode
        Block* b = cache->acquire(to_block_no(i));
        InodeEntry* entry = get_entry(b, i);
        if(entry->type == INODE_INVALID){//找到空闲的inode
            memset(entry, 0, sizeof(InodeEntry));
            entry->type = type;
            cache->sync(ctx, b);
            cache->release(b);
            return i;
        }
        cache->release(b);
    }
    PANIC();
    return 0;
}

// see `inode.h`.
static void inode_lock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    // TODO
    ASSERT(wait_sem(&inode->lock));
}

// see `inode.h`.
static void inode_unlock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    // TODO
    post_sem(&inode->lock);
}

// see `inode.h`.
static void inode_sync(OpContext* ctx, Inode* inode, bool do_write) {
    // TODO
    Block* b = cache->acquire(to_block_no(inode->inode_no));
    InodeEntry* entry = get_entry(b, inode->inode_no);
    //如果inode是有效的，写回
    if(inode->valid && do_write){
        memmove(entry, &inode->entry, sizeof(InodeEntry));
        cache->sync(ctx, b);
    }else if(!inode->valid && !do_write){
        //如果inode无效 并且不需要写回
        memmove(&inode->entry, entry, sizeof(InodeEntry));
        inode->valid = true;
    }
    if (do_write && !inode->valid) {
        PANIC();
    }
    cache->release(b);
}

// see `inode.h`.
static Inode* inode_get(usize inode_no) {
    ASSERT(inode_no > 0);
    ASSERT(inode_no < sblock->num_inodes);
    acquire_spinlock(&lock);
    // TODO
    Inode* inode;

    _for_in_list(p, &head){
        if(p == &head) continue;
        inode = container_of(p, Inode, node);
        if(inode->inode_no == inode_no){
            increment_rc(&inode->rc);
            //增加引用计数
            release_spinlock(&lock);
            inode_lock(inode);
            inode_unlock(inode);
            return inode;
        }
    }

    //没找到inode
    inode = kalloc(sizeof(Inode));
    init_inode(inode);
    inode->inode_no = inode_no;
    increment_rc(&inode->rc);
    _insert_into_list(&head, &inode->node);

    //加载到内存
    inode_lock(inode);
    release_spinlock(&lock);
    inode_sync(NULL, inode, false);
    inode_unlock(inode);  

    //确保加载的 inode 类型不是无效类型（INODE_INVALID），避免返回无效的 inode。
    ASSERT(inode->entry.type != INODE_INVALID);
    return inode;  

    return NULL;
}
// see `inode.h`.
static void inode_clear(OpContext* ctx, Inode* inode) {
    // TODO
    //清空直接地址块
    auto entry = &inode->entry;
    for(u32 i = 0; i < INODE_NUM_DIRECT; i++){
        if(entry->addrs[i] != NULL){
            cache->free(ctx, entry->addrs[i]);
            entry->addrs[i] = NULL;
        }
    }
    //清空间接地址块
    if(entry->indirect != NULL){
        auto b = cache->acquire(entry->indirect);
        auto addrs = get_addrs(b);
        for(usize i = 0; i < INODE_NUM_INDIRECT; i++){
            if(addrs[i] != NULL){
                cache->free(ctx, addrs[i]);
            }
        }
        cache->release(b);
        cache->free(ctx, entry->indirect);
        entry->indirect = NULL;
    }
    //重置inode信息
    entry->num_bytes = 0;
    inode_sync(ctx, inode, true);
}

// see `inode.h`.
static Inode* inode_share(Inode* inode) {
    // TODO
    //增加引用计数
    acquire_spinlock(&lock);
    increment_rc(&inode->rc);
    release_spinlock(&lock);
    return inode;
}

// see `inode.h`.
static void inode_put(OpContext* ctx, Inode* inode) {
    // TODO
    acquire_spinlock(&lock);
    //引用计数检查，确保未被引用并且put后变为0
    if(inode->rc.count == 1 && inode->entry.num_links == 0 && inode->valid){
        //从全局 inode 链表中将该 inode 移除
        _detach_from_list(&inode->node);
        inode_lock(inode);
        release_spinlock(&lock);
        //清理 inode相关资源
        inode_clear(ctx, inode);
        inode->entry.type = INODE_INVALID;
        inode_sync(ctx, inode, true);
        inode->valid = false;
        inode_unlock(inode);
        kfree(inode);
        return;
    }
    //引用计数减少（操作后应该变为0）
    decrement_rc(&inode->rc);
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
static usize inode_map(OpContext* ctx,
                       Inode* inode,
                       usize offset,
                       bool* modified) {
    // TODO
    //这里的偏移量 offset 不是字节偏移量，是块号
    u32 block_no;
    auto entry = &inode->entry;
    *modified = false;
    usize block_number = offset / BLOCK_SIZE;
    //如果偏移量对应直接块
    if(block_number < INODE_NUM_DIRECT){
        if(entry->addrs[block_number] == NULL){
            entry->addrs[block_number] = cache->alloc(ctx);
            *modified = true;
        }
        block_no = entry->addrs[block_number];
    }else if(block_number < INODE_NUM_DIRECT + INODE_NUM_INDIRECT){
        //如果偏移量对应间接块
        block_number -= INODE_NUM_DIRECT;
        //首先检查 entry->indirect 是否为空。如果为空，则需要分配一个新的间接块来存储更多的块地址。
        if(entry->indirect == NULL){
            entry->indirect = cache->alloc(ctx);
        }
        auto b = cache->acquire(entry->indirect);
        auto addrs = get_addrs(b);
        if(addrs[block_number] == NULL){
            addrs[block_number] = cache->alloc(ctx);
            cache->sync(ctx, b);
            *modified = true;
        }
        block_no = addrs[block_number];
        cache->release(b);
    }else{
        PANIC();
    }
    return block_no;
}

// see `inode.h`.
static usize inode_read(Inode* inode, u8* dest, usize offset, usize count) {
    InodeEntry* entry = &inode->entry;
    if (count + offset > entry->num_bytes)
        count = entry->num_bytes - offset;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= entry->num_bytes);
    ASSERT(offset <= end);

    // TODO
    if(inode->entry.type == INODE_DEVICE){
        return console_read(inode, (char*)dest, count);
    }    

    if(count == 0) return count;
    count = 0;
    for(usize i = offset/BLOCK_SIZE; i <= (end-1)/BLOCK_SIZE; i++){
        usize n = MIN(end - offset, (i + 1) * BLOCK_SIZE - offset);
        bool modified;
        auto block_no = inode_map(NULL, inode, offset, &modified);
        // auto block_no = inode_map(NULL, inode, i, &modified);
        auto b = cache->acquire(block_no);
        memmove(dest + count, b->data + offset % BLOCK_SIZE, n);
        cache->release(b);
        offset += n;
        count += n;
    }
    return count;
}

// see `inode.h`.
static usize inode_write(OpContext* ctx,
                         Inode* inode,
                         u8* src,
                         usize offset,
                         usize count) {
    InodeEntry* entry = &inode->entry;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= INODE_MAX_BYTES);
    ASSERT(offset <= end);

    // TODO
    if(inode->entry.type == INODE_DEVICE){
        return console_write(inode, (char*)src, count);
    }


    //通过计算偏移量 offset 和结束位置 end 所在的块号范围，逐块处理数据的写
    count = 0;
    for(usize i = offset/BLOCK_SIZE; i <= (end-1)/BLOCK_SIZE; i++){
        usize n = MIN(end - offset, (i + 1) * BLOCK_SIZE - offset);
        bool modified;
        //获取与当前块号（i）对应的物理块号
        auto block_no = inode_map(ctx, inode, offset, &modified);
        // auto block_no = inode_map(ctx, inode, i, &modified);
        //数据写入
        auto b = cache->acquire(block_no);
        memmove(b->data + offset % BLOCK_SIZE, src + count, n);
        cache->sync(ctx, b);
        cache->release(b);
        offset += n;
        count += n;
    }
    //更新 inode 的 num_bytes
    if(end > entry->num_bytes){
        entry->num_bytes = end;
        inode_sync(ctx, inode, true);
    }
    //返回已写入字节数
    return count;
}

// see `inode.h`.
static usize inode_lookup(Inode* inode, const char* name, usize* index) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    //类似于文件夹的查找，在当前文件夹的目录下面查找文件名为 name 的文件
    for(usize offset = 0; offset < entry->num_bytes; offset += sizeof(DirEntry)){
        DirEntry de;
        inode_read(inode, (u8*)&de, offset, sizeof(DirEntry));
        if(de.inode_no != 0 && strncmp(name, de.name, FILE_NAME_MAX_LENGTH) == 0){
            if(index != NULL) *index = offset;
            return de.inode_no;
        }
    }

    return 0;
}

// see `inode.h`.
static usize inode_insert(OpContext* ctx,
                          Inode* inode,
                          const char* name,
                          usize inode_no) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    usize index;
    if(inode_lookup(inode, name, &index) != 0){
        return -1;
    }
    //找到空闲的目录项
    DirEntry de;
    u32 offset = 0;
    for(offset = 0; offset < entry->num_bytes; offset += sizeof(DirEntry)){
        inode_read(inode, (u8*)&de, offset, sizeof(DirEntry));
        if(de.inode_no == 0){
            break;
        }
    }
    //如果目录项已满，需要增加目录项
    de.inode_no = inode_no;
    memmove(de.name, name, FILE_NAME_MAX_LENGTH);
    inode_write(ctx, inode, (u8*)&de, offset, sizeof(DirEntry));
    return offset;
}

// see `inode.h`.
static void inode_remove(OpContext* ctx, Inode* inode, usize index) {
    // TODO
    ASSERT(index%sizeof(DirEntry) == 0);
    if(index < inode->entry.num_bytes){
        DirEntry de = {0};
        inode_write(ctx, inode, (u8*)&de, index, sizeof(DirEntry));
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
static const char* skipelem(const char* path, char* name) {
    const char* s;
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
static Inode* namex(const char* path,
                    bool nameiparent,
                    char* name,
                    OpContext* ctx) {
    /* (Final) TODO BEGIN */
    if(strncmp(path, "/", 2) == 0){
        return inodes.get(inodes.root->inode_no);
    }
    Inode* ret;
    //设置起始目录
    if(path[0] == '.' || path[0] != '/')ret = inodes.get(thisproc()->cwd->inode_no);
    else ret = inodes.get(inodes.root->inode_no);

    usize index;
    name[0] = 0;
    path = skipelem(path, name);
    if(path == NULL){
        inodes.put(ctx, ret);
        return NULL;
    }
    //遍历目录
    while(path[0] != '\0'){
        inodes.lock(ret);
        usize ino = inodes.lookup(ret, name, &index);
        inodes.unlock(ret);
        inodes.put(ctx, ret);
        if(ino == 0)return NULL;
        ret = inodes.get(ino);
        path = skipelem(path, name);
    }
    if(!nameiparent){
        inodes.lock(ret);
        usize ino = inodes.lookup(ret, name, &index);
        inodes.unlock(ret);
        inodes.put(ctx, ret);
        if(ino == 0)return NULL;
        ret = inodes.get(ino);
        name = NULL;
    }
    return ret; 
    /* (Final) TODO END */
    return 0;
}

Inode* namei(const char* path, OpContext* ctx) {
    char name[FILE_NAME_MAX_LENGTH];
    return namex(path, false, name, ctx);
}

Inode* nameiparent(const char* path, char* name, OpContext* ctx) {
    return namex(path, true, name, ctx);
}

/**
    @brief get the stat information of `ip` into `st`.
    
    @note the caller must hold the lock of `ip`.
 */
void stati(Inode* ip, struct stat* st) {
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