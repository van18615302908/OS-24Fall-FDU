#include <kernel/mem.h>
#include <kernel/sched.h>
#include <fs/pipe.h>
#include <common/string.h>

int pipe_alloc(File** f0, File** f1) {
    // TODO
    *f0 = file_alloc();
    if(*f0 == NULL)return -1;
    *f1 = file_alloc();
    if(*f1 == NULL){
        file_close(*f0);
        return -1;
    }

    // init pipe
    auto p = (Pipe*)kalloc(sizeof(Pipe));
    if(p == NULL){
        file_close(*f0);
        file_close(*f1);
        return -1;
    }
    init_spinlock(&p->lock);
    init_sem(&p->rlock, 0);
    init_sem(&p->wlock, 0);
    p->nread = p->nwrite = 0;
    p->readopen = p->writeopen = true;

    // init files of the pipe
    (*f0)->pipe = (*f1)->pipe = p;
    (*f0)->type = (*f1)->type = FD_PIPE;
    (*f0)->off = (*f1)->off = 0;
    (*f0)->readable = true;
    (*f0)->writable = false;
    (*f1)->readable = false;
    (*f1)->writable = true;
    return 0;
}

void pipe_close(Pipe* pi, int writable) {
    // TODO
    acquire_spinlock(&pi->lock);
    if(writable){
        // close the read end
        pi->writeopen = false;
        post_all_sem(&pi->rlock);
    }
    else{
        // close the write end
        pi->readopen = false;
        post_all_sem(&pi->wlock);
    }
    if(pi->readopen == false && pi->writeopen == false){
        // both ends closed, free the pipe
        release_spinlock(&pi->lock);
        kfree(pi);
        return;
    }
    release_spinlock(&pi->lock);
}

int pipe_write(Pipe* pi, u64 addr, int n) {
    // TODO
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
}

int pipe_read(Pipe* pi, u64 addr, int n) {
    // TODO
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
}