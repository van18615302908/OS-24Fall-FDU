#include "file.h"
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/sem.h>
#include <fs/inode.h>
#include <common/list.h>
#include <kernel/mem.h>

// the global file table.
static struct ftable ftable;

void init_ftable() {
    // TODO: initialize your ftable.
    init_spinlock(&ftable.lock);
}

void init_oftable(struct oftable *oftable) {
    // TODO: initialize your oftable for a new process.
    for(int i=0; i<NOFILE; i++){
        oftable->fp[i] = NULL;
    }
}

/* Allocate a file structure. */
struct file* file_alloc() {
    /* (Final) TODO BEGIN */
    _acquire_spinlock(&ftable.lock);
    for(int i=0; i<NOFILE; i++){
        if(ftable.file[i].ref <= 0){
            ftable.file[i].ref = 1;
            _release_spinlock(&ftable.lock);
            return &ftable.file[i];
        }
    }
    _release_spinlock(&ftable.lock);
    /* (Final) TODO END */
    return 0;
}

/* Increment ref count for file f. */
struct file* file_dup(struct file* f) {
    /* (Final) TODO BEGIN */
    _acquire_spinlock(&ftable.lock);
    f->ref++;
    _release_spinlock(&ftable.lock);
    /* (Final) TODO END */
    return f;
}

/* Close file f. (Decrement ref count, close when reaches 0.) */
void file_close(struct file* f) {
    /* (Final) TODO BEGIN */
    _acquire_spinlock(&ftable.lock);
    f->ref--;
    if(f->ref == 0){
        if(f->type == FD_INODE){
            Inode* inode = f->ip;
            f->type = FD_NONE;
            _release_spinlock(&ftable.lock);
            OpContext ctx;
            bcache.begin_op(&ctx);
            inodes.put(&ctx, inode);
            bcache.end_op(&ctx);
        }else if(f->type == FD_PIPE){
            pipeClose(f->pipe, f->writable);
            _release_spinlock(&ftable.lock);
        }
        return;
    }
    _release_spinlock(&ftable.lock);
    /* (Final) TODO END */
}

/* Get metadata about file f. */
int file_stat(struct file* f, struct stat* st) {
    /* (Final) TODO BEGIN */
    if(f->type == FD_INODE){
        inodes.lock(f->ip);
        stati(f->ip, st);
        inodes.unlock(f->ip);
        return 0;
    }
    /* (Final) TODO END */
    return -1;
}

/* Read from file f. */
isize file_read(struct file* f, char* addr, isize n) {
    /* (Final) TODO BEGIN */
    if(f->type == FD_INODE && f->readable){
        inodes.lock(f->ip);
        n = inodes.read(f->ip, (u8*)addr, f->off, n);
        f->off += n;
        inodes.unlock(f->ip);
        return n;
    }else if(f->type == FD_PIPE && f->readable){
        return pipeRead(f->pipe, (u64)addr, n);
    }  
    /* (Final) TODO END */
    return 0;
}

/* Write to file f. */
isize file_write(struct file* f, char* addr, isize n) {
    /* (Final) TODO */
    if(!f->writable || f->type == FD_NONE || n < 0)return -1;
    isize ret = 0;
    if(f->type == FD_INODE){
        ASSERT(f->ip->inode_no > 9);
        usize max_input = MIN(INODE_MAX_BYTES - f->off, (usize)n);
        usize n_w = 0;
        while(n_w != max_input){
            Assert(n_w <= max_input);//防止死循环
            usize this = MIN(max_input - n_w, (usize)(OP_MAX_NUM_BLOCKS * BLOCK_SIZE / 2));
            OpContext ctx;
            bcache.begin_op(&ctx);
            inodes.lock(f->ip);
            if(inodes.write(&ctx, f->ip, (u8*)(addr + n_w), f->off, this) != this){
                inodes.unlock(f->ip);
                bcache.end_op(&ctx);
                return -1;
            };
            inodes.unlock(f->ip);
            bcache.end_op(&ctx);
            f->off += this;
            n_w += this;
            ret += this;
        }
    }
    else if(f->type == FD_PIPE){
        ret = pipeWrite(f->pipe, (u64)addr, n);
    }
    return ret;
}