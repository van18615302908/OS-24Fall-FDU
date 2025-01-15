#include <kernel/mem.h>
#include <kernel/sched.h>
#include <fs/pipe.h>
#include <common/string.h>
#include <kernel/printk.h>

void init_pipe(Pipe *pi)
{
    /* (Final) TODO BEGIN */
    // 初始化自旋锁
    init_spinlock(&pi->lock);

    // 初始化信号量，用于控制读写同步
    init_sem(&pi->rlock, 0);
    init_sem(&pi->wlock, 0);

    // 初始化缓冲区相关状态
    pi->nread = 0;       // 已读取的字节数
    pi->nwrite = 0;      // 已写入的字节数

    // 初始化管道的打开状态
    pi->readopen = 1;    // 读端打开
    pi->writeopen = 1;   // 写端打开
    /* (Final) TODO END */
}

void init_read_pipe(File *readp, Pipe *pipe)
{
    /* (Final) TODO BEGIN */
    // 设置文件类型为管道
    readp->type = FD_PIPE;

    // 设置文件是否可读/可写
    readp->readable = true;   // 读端可读
    readp->writable = false;  // 读端不可写

    // 将文件与管道关联
    readp->pipe = pipe;

    // 初始化引用计数
    readp->ref = 1;

    // 对于管道，偏移量不是文件指针，而是用于记录已读字节数
    readp->off = 0;  // 初始化为 0，表示尚未读取任何字节
    /* (Final) TODO END */
}

void init_write_pipe(File *writep, Pipe *pipe)
{
    /* (Final) TODO BEGIN */
    // 设置文件类型为管道
    writep->type = FD_PIPE;

    // 设置文件是否可读/可写
    writep->readable = false;  // 写端不可读
    writep->writable = true;   // 写端可写

    // 将文件与管道关联
    writep->pipe = pipe;

    // 初始化引用计数
    writep->ref = 1;

    // 对于管道，偏移量不是文件指针，而是用于记录已写字节数
    writep->off = 0;  // 初始化为 0，表示尚未写入任何字节
    /* (Final) TODO END */
}

int pipe_alloc(File **f0, File **f1)
{
    /* (Final) TODO BEGIN */
    *f0 = file_alloc();
    if(*f0 == NULL)return -1;
    *f1 = file_alloc();
    if(*f1 == NULL){
        file_close(*f0);
        return -1;
    }
    Pipe *pi = (Pipe *)kalloc(sizeof(Pipe));
    if(pi == NULL){
        file_close(*f0);
        file_close(*f1);
        return -1;
    }
    init_pipe(pi);
    init_read_pipe(*f0, pi);
    init_write_pipe(*f1, pi);
    return 0;
    /* (Final) TODO END */
}

void pipe_close(Pipe *pi, int writable)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);
    if(writable){
        pi->writeopen = false;
        post_all_sem(&pi->rlock);
    }
    else{
        pi->readopen = false;
        post_all_sem(&pi->wlock);
    }
    if(pi->readopen == false && pi->writeopen == false){
        release_spinlock(&pi->lock);
        kfree(pi);
        return;
    }
    release_spinlock(&pi->lock); 
    /* (Final) TODO END */
}

int pipe_write(Pipe *pi, u64 addr, int n)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);
    if(!pi->writeopen){
        release_spinlock(&pi->lock);
        return -1;
    }

    int ret = 0;
    while(ret < n){
        if(!pi->readopen){
            release_spinlock(&pi->lock);
            return -1;
        }
        if(pi->nwrite - pi->nread >= PIPE_SIZE){
            post_all_sem(&pi->rlock);
            release_spinlock(&pi->lock);
            if(!_wait_sem(&pi->wlock, true)){
                return ret;
            }
            acquire_spinlock(&pi->lock);
        }
        else{
            pi->data[pi->nwrite++ % PIPE_SIZE] = ((char*)addr)[ret++];
        }
    }
    post_all_sem(&pi->rlock);
    release_spinlock(&pi->lock);
    return ret;
    /* (Final) TODO END */
}

int pipe_read(Pipe *pi, u64 addr, int n)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);

    if(!pi->readopen){
        release_spinlock(&pi->lock);
        return -1;
    }
    
    while (pi->nwrite == pi->nread && pi->writeopen) {
        release_spinlock(&pi->lock);
        if(_wait_sem(&pi->rlock, true) == false){
            return -1;
        }
        acquire_spinlock(&pi->lock);
    }

    int ret = 0;
    while(ret < n){
        if(pi->nwrite == pi->nread)break;
        ((char*)addr)[ret++] = pi->data[pi->nread++ % PIPE_SIZE];
    }
    post_all_sem(&pi->wlock);
    release_spinlock(&pi->lock);
    return ret;
    /* (Final) TODO END */
}