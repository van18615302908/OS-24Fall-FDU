#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <stddef.h>
#include "syscall.h"
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/string.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>



/** 
 * Get the file object by fd. Return null if the fd is invalid.
 */
static struct file *fd2file(int fd)
{
    /* (Final) TODO BEGIN */
    if (fd >= NFILE || fd < 0) {
        return NULL;
    }
    File *file = thisproc()->oftable.files[fd];
    if (file == NULL || file->type == FD_NONE) {
        return NULL;
    }

    return file;

    /* (Final) TODO END */
}

/*
 * Allocate a file descriptor for the given file.
 * Takes over file reference from caller on success.
 */
int fdalloc(struct file *f)
{
    /* (Final) TODO BEGIN */

    Proc *this = thisproc();
    acquire_spinlock(&this->oftable.lock);
    for (usize i = 0; i < NFILE_PROC; i++) {
        if (this->oftable.files[i] == NULL) {
            this->oftable.files[i] = f;
            release_spinlock(&this->oftable.lock);
            return i;
        }
    }
    release_spinlock(&this->oftable.lock);

    /* (Final) TODO END */
    return -1;
}

define_syscall(ioctl, int fd, u64 request)
{
    // 0x5413 is TIOCGWINSZ (I/O Control to Get the WINdow SIZe, a magic request
    // to get the stdin terminal size) in our implementation. Just ignore it.
    ASSERT(request == 0x5413);
    (void)fd;
    return 0;
}
u64 find_free_memory_region(ListNode *section_head, int length) {
    u64 search_start = 0x70000000; // 从 0x70000000 开始搜索
    u64 begin = search_start;
    u64 end = begin + length;

    while (true) {
        bool valid = true;
        ListNode *node = section_head->next;

        while (node != section_head) {
            struct section *section = container_of(node, struct section, stnode);
            if (section->begin < end && section->end > begin) {
                // 调整搜索起点
                begin = ALIGN_UP(section->end, PAGE_SIZE);
                end = begin + length;
                valid = false;
                break;
            }
            node = node->next;
        }

        if (valid) break;
    }

    return begin;
}
bool is_address_valid(ListNode *section_head, u64 begin, u64 end) {
    ListNode *node = section_head->next;
    while (node != section_head) {
        struct section *section = container_of(node, struct section, stnode);
        if (section->begin < end && section->end > begin) {
            return false; // 地址冲突
        }
        node = node->next;
    }
    return true;
}
struct section* create_mmap_section(File *f, u64 begin, int length, int prot, int flags, int offset) {
    struct section *map_section = (struct section *)kalloc(sizeof(struct section));
    if (!map_section) {
        printk("(warn) mmap: failed to allocate section\n");
        return NULL;
    }

    map_section->begin = begin;
    map_section->end = begin + length;
    map_section->flags = (flags == MAP_PRIVATE ? ST_MMAP_PRIVATE : ST_MMAP_SHARED);
    map_section->fp = file_dup(f);
    map_section->offset = offset;
    map_section->length = length;
    map_section->prot = prot;

    return map_section;
}
define_syscall(mmap, void *addr, int length, int prot, int flags, int fd, int offset) {
    File *f = fd2file(fd);
    if (!f) {
        printk("mmap: file doesn't exist!\n");
        return -1;
    }

    // 检查权限
    if ((prot & PROT_WRITE) && flags != MAP_PRIVATE && !f->writable) {
        printk("mmap: creating shared writable mmap but file isn't writable!\n");
        return -1;
    }

    Proc *this = thisproc();
    acquire_spinlock(&this->pgdir.lock);

    u64 begin, end;
    if (!addr) {
        // 动态分配地址
        begin = find_free_memory_region(&this->pgdir.section_head, length);
        if (begin == 0) {
            release_spinlock(&this->pgdir.lock);
            printk("mmap: cannot find appropriate space for mmap\n");
            return -1;
        }
    } else {
        // 使用用户提供的地址
        begin = (u64)addr;
        end = begin + length;
        if (!is_address_valid(&this->pgdir.section_head, begin, end)) {
            release_spinlock(&this->pgdir.lock);
            printk("mmap: given address intersects with existing sections\n");
            return -1;
        }
    }

    // 创建映射段
    struct section *map_section = create_mmap_section(f, begin, length, prot, flags, offset);
    if (!map_section) {
        release_spinlock(&this->pgdir.lock);
        return -1;
    }

    // 插入到段列表
    _insert_into_list(&this->pgdir.section_head, &map_section->stnode);
    release_spinlock(&this->pgdir.lock);

    return begin;
}

struct section* find_mapped_section(ListNode *section_head, u64 addr) {
    ListNode *node = section_head->next;
    while (node != section_head) {
        struct section *section = container_of(node, struct section, stnode);
        if (section->begin == addr) {
            return section;
        }
        node = node->next;
    }
    return NULL; // 未找到映射段
}
void release_mapped_pages(struct pgdir *pgdir, struct section *mapped_section, size_t length, bool free_whole_section) {
    u64 va = ALIGN_DOWN(mapped_section->begin, PAGE_SIZE);
    u64 end_va = free_whole_section ? mapped_section->end : mapped_section->begin + length;

    while (va < end_va) {
        PTEntriesPtr pte = get_pte(pgdir, va, false);
        if (!pte) {
            va += PAGE_SIZE;
            continue;
        }

        if (CHECK_DESCRIPTOR(*pte)) {
            void *old_page = (void *)P2K(PTE_ADDRESS(*pte));
            kfree_page(old_page); // 释放物理页
        }

        *pte = 0; // 清除页表项
        va += PAGE_SIZE;
    }
}
define_syscall(munmap, void *addr, size_t length) {
    Proc *this = thisproc();
    acquire_spinlock(&this->pgdir.lock);

    // 查找对应的映射段
    struct section *mapped_section = find_mapped_section(&this->pgdir.section_head, (u64)addr);
    if (!mapped_section || !mapped_section->fp) {
        release_spinlock(&this->pgdir.lock);
        return 0; // 如果未找到映射，直接返回
    }

    // 确定需要释放的长度
    bool free_whole_section = false;
    if (length >= mapped_section->end - mapped_section->begin) {
        length = mapped_section->end - mapped_section->begin;
        free_whole_section = true;
    }

    // 写回共享映射区域
    if (mapped_section->flags == ST_MMAP_SHARED && (mapped_section->prot & PROT_WRITE)) {
        write_back(&this->pgdir, mapped_section->fp, mapped_section->begin, mapped_section->offset, length);
    }

    // 释放虚拟地址对应的页表映射
    release_mapped_pages(&this->pgdir, mapped_section, length, free_whole_section);

    if (free_whole_section) {
        // 从段列表中移除映射段并释放资源
        _detach_from_list(&mapped_section->stnode);
        file_close(mapped_section->fp);
        kfree(mapped_section);
    } else {
        // 更新部分释放后的映射段信息
        mapped_section->begin += length;
        mapped_section->offset += length;
        mapped_section->length -= length;
    }

    arch_tlbi_vmalle1is(); // 刷新 TLB
    release_spinlock(&this->pgdir.lock);
    return 0;
}

define_syscall(dup, int fd)
{
    struct file *f = fd2file(fd);
    if (!f)return -1;
    fd = fdalloc(f);
    if (fd < 0)return -1;
    file_dup(f);
    return fd;
}

define_syscall(read, int fd, char *buffer, int size)
{
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_writeable(buffer, size))return -1;
    return file_read(f, buffer, size);
}

define_syscall(write, int fd, char *buffer, int size)
{
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_readable(buffer, size))return -1;
    return file_write(f, buffer, size);
}

define_syscall(writev, int fd, struct iovec *iov, int iovcnt)
{
    struct file *f = fd2file(fd);
    struct iovec *p;
    if (!f || iovcnt <= 0 || !user_readable(iov, sizeof(struct iovec) * iovcnt))return -1;
    usize tot = 0;
    for (p = iov; p < iov + iovcnt; p++) {
        if (!user_readable(p->iov_base, p->iov_len))return -1;
        tot += file_write(f, p->iov_base, p->iov_len);
    }
    return tot;
}

define_syscall(close, int fd)
{
    /* (Final) TODO BEGIN */
    File *f = fd2file(fd);

    if (f == NULL) return -1;

    thisproc()->oftable.files[fd] = 0;
    file_close(f);

    /* (Final) TODO END */
    return 0;
}

define_syscall(fstat, int fd, struct stat *st)
{
    struct file *f = fd2file(fd);
    if (!f || !user_writeable(st, sizeof(*st)))return -1;
    return file_stat(f, st);
}

define_syscall(newfstatat, int dirfd, const char *path, struct stat *st,
               int flags)
{
    if (!user_strlen(path, 256) || !user_writeable(st, sizeof(*st)))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_fstatat: dirfd unimplemented\n");
        return -1;
    }
    if (flags != 0) {
        printk("sys_fstatat: flags unimplemented\n");
        return -1;
    }

    Inode *ip;
    OpContext ctx;
    bcache.begin_op(&ctx);

    if ((ip = namei(path, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.lock(ip);
    stati(ip, st);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);

    return 0;
}
bool validate_directory(OpContext *ctx, Inode *inode) {
    inodes.lock(inode);

    if (inode->entry.type != INODE_DIRECTORY) {
        inodes.unlock(inode);
        inodes.put(ctx, inode);
        return false; // 非目录类型
    }

    inodes.unlock(inode);
    return true; // 验证通过
}
void update_cwd(Proc *proc, OpContext *ctx, Inode *new_cwd) {
    if (proc->cwd) {
        inodes.put(ctx, proc->cwd); // 释放当前工作目录的引用
    }
    proc->cwd = new_cwd; // 设置新的工作目录
}
static int isdirempty(Inode *dp)
{
    usize off;
    DirEntry de;

    for (off = 2 * sizeof(de); off < dp->entry.num_bytes; off += sizeof(de)) {
        if (inodes.read(dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))PANIC();
        if (de.inode_no != 0)
            return 0;
    }
    return 1;
}

define_syscall(unlinkat, int fd, const char *path, int flag)
{
    ASSERT(fd == AT_FDCWD && flag == 0);
    Inode *ip, *dp;
    DirEntry de;
    char name[FILE_NAME_MAX_LENGTH];
    usize index;
    if (!user_strlen(path, 256))
        return -1;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((dp = nameiparent(path, name, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.lock(dp);

    // Cannot unlink "." or "..".
    if (strncmp(name, ".", FILE_NAME_MAX_LENGTH) == 0 ||
        strncmp(name, "..", FILE_NAME_MAX_LENGTH) == 0)
        goto bad;

    usize inumber = inodes.lookup(dp, name, &index);
    if (inumber == 0)
        goto bad;
    ip = inodes.get(inumber);
    inodes.lock(ip);

    if (ip->entry.num_links < 1)
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY && !isdirempty(ip)) {
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        goto bad;
    }

    memset(&de, 0, sizeof(de));
    if (inodes.write(&ctx, dp, (u8 *)&de, sizeof(de) * index, sizeof(de)) !=
        sizeof(de))
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY) {
        dp->entry.num_links--;
        inodes.sync(&ctx, dp, true);
    }
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    ip->entry.num_links--;
    inodes.sync(&ctx, ip, true);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;

bad:
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    bcache.end_op(&ctx);
    return -1;
}


Inode *handle_existing_inode(OpContext *ctx, Inode *parent, usize inode_index, short type) {
    inodes.unlock(parent);
    inodes.put(ctx, parent);

    Inode *target = inodes.get(inode_index);
    inodes.lock(target);

    if (type == target->entry.type) {
        return target; // 类型匹配，直接返回
    }

    inodes.unlock(target);
    inodes.put(ctx, target);
    return NULL; // 类型不匹配
}
Inode *initialize_inode(OpContext *ctx, usize inode_index, short type, short major, short minor) {
    Inode *target = inodes.get(inode_index);
    inodes.lock(target);

    target->entry.type = type;
    target->entry.major = major;
    target->entry.minor = minor;
    target->entry.num_links = 1;
    inodes.sync(ctx, target, true);

    return target;
}
bool create_dot_entries(OpContext *ctx, Inode *target, Inode *parent) {
    if (inodes.insert(ctx, target, ".", target->inode_no) < 0 ||
        inodes.insert(ctx, target, "..", parent->inode_no) < 0) {
        printk("(warn) failed to alloc . or ..\n");
        return false; // 创建失败
    }

    parent->entry.num_links++; // 增加父目录的引用计数
    inodes.sync(ctx, parent, true);
    return true;
}
void cleanup_parent(OpContext *ctx, Inode *parent) {
    inodes.unlock(parent);
    inodes.put(ctx, parent);
}
void cleanup_target(OpContext *ctx, Inode *parent, Inode *target) {
    cleanup_parent(ctx, parent);
    inodes.clear(ctx, target);
    inodes.unlock(target);
    inodes.put(ctx, target);
}

/**
    @brief create an inode at `path` with `type`.

    If the inode exists, just return it.

    If `type` is directory, you should also create "." and ".." entries and link
   them with the new inode.

    @note BE careful of handling error! You should clean up ALL the resources
   you allocated and free ALL acquired locks when error occurs. e.g. if you
   allocate a new inode "/my/dir", but failed to create ".", you should free the
   inode "/my/dir" before return.

    @see `nameiparent` will find the parent directory of `path`.

    @return Inode* the created inode, or NULL if failed.
 */
Inode *create(const char *path, short type, short major, short minor, OpContext *ctx) {
    char name[FILE_NAME_MAX_LENGTH];

    // 获取父目录 inode
    Inode *parent = nameiparent(path, name, ctx);
    if (!parent) return NULL;

    inodes.lock(parent);

    // 检查父目录是否已存在目标文件
    usize inode_index = inodes.lookup(parent, name, NULL);
    if (inode_index > 0) {
        return handle_existing_inode(ctx, parent, inode_index, type);
    }

    // 分配新的 inode
    inode_index = inodes.alloc(ctx, type);
    if (inode_index == 0) {
        printk("PANIC: failed to alloc inode\n");
        cleanup_parent(ctx, parent);
        return NULL;
    }

    // 初始化新 inode
    Inode *target = initialize_inode(ctx, inode_index, type, major, minor);

    // 如果是目录，创建 `.` 和 `..`
    if (type == INODE_DIRECTORY && !create_dot_entries(ctx, target, parent)) {
        cleanup_target(ctx, parent, target);
        return NULL;
    }

    // 将新 inode 插入到父目录
    if (inodes.insert(ctx, parent, name, target->inode_no) < 0) {
        printk("(warn) failed to append new entry to parent\n");
        cleanup_target(ctx, parent, target);
        return NULL;
    }

    cleanup_parent(ctx, parent);
    return target;
}


define_syscall(openat, int dirfd, const char *path, int omode)
{
    int fd;
    struct file *f;
    Inode *ip;

    if (!user_strlen(path, 256))
        return -1;

    if (dirfd != AT_FDCWD) {
        printk("sys_openat: dirfd unimplemented\n");
        return -1;
    }

    OpContext ctx;
    bcache.begin_op(&ctx);
    if (omode & O_CREAT) {
        // FIXME: Support acl mode.
        ip = create(path, INODE_REGULAR, 0, 0, &ctx);
        if (ip == 0) {
            bcache.end_op(&ctx);
            return -1;
        }
    } else {
        if ((ip = namei(path, &ctx)) == 0) {
            bcache.end_op(&ctx);
            return -1;
        }
        inodes.lock(ip);
    }

    if ((f = file_alloc()) == 0 || (fd = fdalloc(f)) < 0) {
        if (f)
            file_close(f);
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    bcache.end_op(&ctx);

    f->type = FD_INODE;
    f->ip = ip;
     f->off = (omode & O_APPEND) ? ip->entry.num_bytes : 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    return fd;
}

define_syscall(mkdirat, int dirfd, const char *path, int mode)
{
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mkdirat: dirfd unimplemented\n");
        return -1;
    }
    if (mode != 0) {
        printk("sys_mkdirat: mode unimplemented\n");
        return -1;
    }
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DIRECTORY, 0, 0, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

define_syscall(mknodat, int dirfd, const char *path,
               __attribute__((unused)) mode_t mode, dev_t dev)
{
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mknodat: dirfd unimplemented\n");
        return -1;
    }

    unsigned int ma = major(dev);
    unsigned int mi = minor(dev);
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DEVICE, (short)ma, (short)mi, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

define_syscall(chdir, const char *path) {
    Proc *this = thisproc();

    // 开始操作上下文
    OpContext ctx;
    bcache.begin_op(&ctx);

    // 获取目标路径对应的 inode
    Inode *inode = namei(path, &ctx);
    if (!inode) {
        bcache.end_op(&ctx);
        return -1; // 路径无效
    }

    // 验证 inode 是否为目录
    if (!validate_directory(&ctx, inode)) {
        bcache.end_op(&ctx);
        return -1; // 非目录类型
    }

    // 更新当前工作目录
    update_cwd(this, &ctx, inode);

    bcache.end_op(&ctx); // 结束操作上下文
    return 0;
}

define_syscall(pipe2, int pipefd[2], int flags)
{
    /* (Final) TODO BEGIN */
    File *f0, *f1;
    if (pipe_alloc(&f0, &f1) < 0)
        return -1;
    if ((pipefd[0] = fdalloc(f0)) < 0) {
        pipe_close(f0->pipe, FALSE);
        pipe_close(f0->pipe, TRUE);
        file_close(f0);
        file_close(f1);
        return -1;
    }
    if ((pipefd[1] = fdalloc(f1)) < 0) {
        pipe_close(f0->pipe, FALSE);
        pipe_close(f0->pipe, TRUE);
        sys_close(pipefd[0]);
        file_close(f1);
        return -1;
    }
    return 0;
    /* (Final) TODO END */
}