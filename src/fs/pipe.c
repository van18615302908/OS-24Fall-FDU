#include <kernel/mem.h>
#include <kernel/sched.h>
#include <fs/pipe.h>
#include <common/string.h>
#include <kernel/printk.h>

void init_pipe(Pipe *pi)
{
    /* (Final) TODO BEGIN */
    init_spinlock(&pi->lock);
    init_sem(&pi->rlock, 0);
    init_sem(&pi->wlock, 0);
    pi->nread = pi->nwrite = 0;
    pi->readopen = pi->writeopen = TRUE;
    /* (Final) TODO END */
}

void init_read_pipe(File *readp, Pipe *pipe)
{
    /* (Final) TODO BEGIN */
    readp->pipe = pipe;
    readp->type = FD_PIPE;
    readp->off = 0;
    readp->readable = TRUE;
    readp->writable = FALSE;
    /* (Final) TODO END */
}

void init_write_pipe(File *writep, Pipe *pipe)
{
    /* (Final) TODO BEGIN */
    writep->pipe = pipe;
    writep->type = FD_PIPE;
    writep->off = 0;
    writep->readable = FALSE;
    writep->writable = TRUE;
    /* (Final) TODO END */
}

int pipe_alloc(File **f0, File **f1)
{
    /* (Final) TODO BEGIN */
    *f0 = file_alloc();
    if (!*f0) {
        return -1;
    }
    *f1 = file_alloc();
    if (!*f1) {
        file_close(*f0);
        return -1;
    }

    // init pipe
    Pipe *p = (Pipe *)kalloc(sizeof(Pipe));
    if (p == NULL) {
        file_close(*f0);
        file_close(*f1);
        PANIC();
        return -1;
    }

    init_pipe(p);
    init_read_pipe(*f0, p);
    init_write_pipe(*f1, p);
    return 0;
    /* (Final) TODO END */
}

void pipe_close(Pipe *pi, int writable)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);
    if (writable) {
        // end write
        pi->writeopen = FALSE;
        post_all_sem(&pi->rlock);
    } else {
        //end read
        pi->readopen = FALSE;
        post_all_sem(&pi->wlock);
    }
    if (pi->readopen == FALSE && pi->writeopen == FALSE) {
        // free the pipe
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
    // printk("(in pipe_write)\n");
    acquire_spinlock(&pi->lock);
    ASSERT(pi->writeopen);

    int len = 0;
    while (len < n) {
        ASSERT(pi->readopen);
        if (pi->nwrite >= pi->nread + PIPE_SIZE) {
            //buffer is full
            post_all_sem(&pi->rlock);
            release_spinlock(&pi->lock);
            if (!wait_sem(&pi->wlock)) {
                return len;
            }
            acquire_spinlock(&pi->lock);
        } else {
            pi->data[pi->nwrite % PIPE_SIZE] = ((char *)addr)[len];
            pi->nwrite++;
            len++;
        }
    }
    post_all_sem(&pi->rlock);
    release_spinlock(&pi->lock);
    return len;
    /* (Final) TODO END */
}

int pipe_read(Pipe *pi, u64 addr, int n)
{
    /* (Final) TODO BEGIN */
    // printk("(in pipe_read)\n");
    acquire_spinlock(&pi->lock);
    ASSERT(pi->readopen);
    while (pi->nwrite == pi->nread && pi->writeopen) {
        release_spinlock(&pi->lock);
        if (_wait_sem(&pi->rlock, TRUE) == FALSE) {
            return -1;
        }
        acquire_spinlock(&pi->lock);
    }

    int len = 0;
    while (len < n) {
        if (pi->nwrite == pi->nread)
            break;
        ((char *)addr)[len] = pi->data[pi->nread % PIPE_SIZE];
        len++;
        pi->nread++;
    }
    post_all_sem(&pi->wlock);
    release_spinlock(&pi->lock);
    return len;
    /* (Final) TODO END */
}