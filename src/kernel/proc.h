#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/rbtree.h>
#include <kernel/pt.h>

enum procstate { UNUSED, RUNNABLE, RUNNING, SLEEPING, ZOMBIE };//从0开始

typedef struct UserContext {
    // TODO: customize your trap frame
    u64 spsr, elr, sp_el0;
    u64 x[31];//x0-x31
} UserContext;

typedef struct KernelContext {
    // TODO: customize your context
    u64 lr,x0,x1;
    u64 x[11];//x19-29

} KernelContext;

// embeded data for procs
typedef struct schinfo {
    // TODO: customize your sched info
    ListNode rq;//运行队列
} Schinfo;

typedef struct Proc {
    bool killed;
    bool idle;
    int pid;
    int exitcode;
    enum procstate state;
    Semaphore childexit;
    ListNode children;
    ListNode ptnode;
    struct Proc *parent;
    struct schinfo schinfo;
    struct pgdir pgdir;
    void *kstack;
    UserContext *ucontext;
    KernelContext *kcontext;
} Proc;

void init_kproc();
void init_proc(Proc *);
Proc *create_proc();
int start_proc(Proc *, void (*entry)(u64), u64 arg);
NO_RETURN void exit(int code);
int wait(int *exitcode);
int kill(int pid);