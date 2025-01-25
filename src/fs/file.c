#include "file.h"
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/sem.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <common/list.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <common/string.h>


static struct ftable ftable;

void init_ftable()
{
    // TODO: initialize your ftable.
    init_spinlock(&ftable.lock);
    for (usize i = 0; i < NFILE; i++) {
        ftable.files[i].ref = 0;
        ftable.files[i].type = FD_NONE;
    }
}

void init_oftable(struct oftable *oftable)
{
    // TODO: initialize your oftable for a new process.
    init_spinlock(&oftable->lock);
    memset(&oftable->files, 0, sizeof(oftable->files));
}

struct file *file_alloc()
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&ftable.lock);

    for (usize i = 0; i < NFILE; i++) {
        if (ftable.files[i].ref == 0 && ftable.files[i].type == FD_NONE) {
            ftable.files[i].ref++;
            release_spinlock(&ftable.lock);
            return &ftable.files[i];
        }
    }
    release_spinlock(&ftable.lock);

    /* (Final) TODO END */
    PANIC();
    return NULL;
}

/* Increment ref count for file f. */
struct file *file_dup(struct file *f)
{
    /* (Final) TODO BEGIN */

    acquire_spinlock(&ftable.lock);
    f->ref++;
    release_spinlock(&ftable.lock);
    /* (Final) TODO END */
    return f;
}

// 处理管道文件的释放逻辑
static void handle_pipe_file(struct file *f)
{
    struct pipe *pipe = f->pipe;
    ASSERT(f->readable ^ f->writable); // 管道文件只能是可读或可写，不可同时为两者
    release_spinlock(&ftable.lock);
    pipe_close(pipe, f->writable);
}

// 处理 Inode 文件的释放逻辑
static void handle_inode_file(struct file *f)
{
    Inode *inode = f->ip;
    release_spinlock(&ftable.lock);

    OpContext ctx;
    bcache.begin_op(&ctx);
    inodes.put(&ctx, inode); // 释放 Inode
    bcache.end_op(&ctx);
}


/* Close file f. (Decrement ref count, close when reaches 0.) */
void file_close(struct file *f)
{
    /* (Final) TODO BEGIN */

    acquire_spinlock(&ftable.lock);
    f->ref--;
    if (f->ref > 0) {
        release_spinlock(&ftable.lock);
        return;
    }
    if(f->ref < 0) {
        release_spinlock(&ftable.lock);
        printk("file_close: f->ref < 0\n");
        PANIC();
    }

    int file_type = f->type;
    f->type = FD_NONE;

switch (file_type) {
    case FD_PIPE:
        handle_pipe_file(f);
        break;

    case FD_INODE:
        handle_inode_file(f);
        break;

    default:
        release_spinlock(&ftable.lock);
        break;
}

    /* (Final) TODO END */
}

/* Get metadata about file f. */
int file_stat(struct file *f, struct stat *st)
{
    /* (Final) TODO BEGIN */
    if (f->type == FD_INODE) {
        inodes.lock(f->ip);
        stati(f->ip, st);
        inodes.unlock(f->ip);
        return 0;
    }

    /* (Final) TODO END */
    return -1;
}

/* Read from file f. */
isize file_read(struct file *f, char *addr, isize n)
{
    /* (Final) TODO BEGIN */
    if (!f->readable) {
        return -1; 
    }

    if (f->type == FD_INODE) { // 处理 FD_INODE 类型
        ASSERT(f->ip != NULL);
        inodes.lock(f->ip); 
        usize bytes_read = inodes.read(f->ip, (u8 *)addr, f->off, n); 
        f->off += bytes_read; 
        inodes.unlock(f->ip); 
        return bytes_read;
    } else if (f->type == FD_PIPE) { // 处理 FD_PIPE 类型
        return pipe_read(f->pipe, (u64)addr, n); 
    }

    /* (Final) TODO END */
    return 0; 
}

/* 写入到 Inode 文件 */
static isize write_to_inode(struct file *f, char *addr, isize n)
{
    ASSERT(f->ip != NULL);
    inodes.lock(f->ip);

    usize bytes_written = 0;

    // 将写入操作拆分为多个原子操作
    while (n > 0) {
        OpContext ctx;
        bcache.begin_op(&ctx);

        u64 should_write = MIN(n, ((OP_MAX_NUM_BLOCKS - 1) / 2 - 1) * BLOCK_SIZE);
        u64 count = inodes.write(&ctx, f->ip, (u8 *)addr, f->off, should_write);

        bcache.end_op(&ctx);

        bytes_written += count;
        n -= count;
        f->off += count;

        if (count != should_write) {
            printk("(warn) cannot write file, terminating\n");
            break;
        }
    }

    inodes.unlock(f->ip);
    return bytes_written;
}

/* 写入到管道文件 */
static isize write_to_pipe(struct file *f, char *addr, isize n)
{
    return pipe_write(f->pipe, (u64)addr, n);
}


/* Write to file f. */
isize file_write(struct file *f, char *addr, isize n)
{
    /* 检查文件是否可写 */
    if (!f->writable) {
        printk("(warn) Writing unwritable file\n");
        return -1;
    }

    if (f->type == FD_INODE) {
        return write_to_inode(f, addr, n);
    } else if (f->type == FD_PIPE) {
        return write_to_pipe(f, addr, n);
    }

    return 0; // 不支持的文件类型
}