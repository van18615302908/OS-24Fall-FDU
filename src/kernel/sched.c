#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <common/rbtree.h>
#include <driver/timer.h>

extern bool panic_flag;

extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);
int debug_sched = 0;

static SpinLock sched_lock;
static ListNode rq;
static SpinLock rqlock;

void init_sched()
{
    // TODO: initialize the scheduler
    // 1. initialize the resources (e.g. locks, semaphores)
    // 2. initialize the scheduler info of each CPU
    if(debug_sched)printk("init_sched\n");
    init_spinlock(&sched_lock);
    init_list_node(&rq);
    for(int i=0; i<NCPU; i++){
        struct Proc* p = kalloc(sizeof(struct Proc));
        p->idle = true;
        p->state = RUNNING;
        cpus[i].sched.this_proc = cpus[i].sched.idle = p;
    }

}

Proc *thisproc()
{
    if(debug_sched)printk("thisproc\n");
    // TODO: return the current process
    return cpus[cpuid()].sched.this_proc;
}

void init_schinfo(struct schinfo *p)
{
    if(debug_sched)printk("init_schinfo\n");
    // TODO: initialize your customized schinfo for every newly-created process
    init_list_node(&p->rq);
}

void acquire_sched_lock()
{
    // if(debug_sched)printk("acquire_sched_lock\n");
    // TODO: acquire the sched_lock if need
    acquire_spinlock(&sched_lock);
}

void release_sched_lock()
{
    // if(debug_sched)printk("release_sched_lock\n");
    // TODO: release the sched_lock if need
    release_spinlock(&sched_lock);
}

bool is_zombie(Proc *p)
{
    bool r;
    r = p->state == ZOMBIE;
    return r;
}


bool activate_proc(Proc *p)
{
    // TODO:
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    // else: panic
    if(debug_sched)printk("activate_proc\n");
    acquire_sched_lock();
    //如果进程已经是运行态或者可运行态，则不做任何操作
    if(p->state == RUNNING || p->state == RUNNABLE){
        release_sched_lock();
        return false;
    }else if(p->state == SLEEPING || p->state == UNUSED){
        //如果进程是睡眠态或者未使用态，则将其设置为可运行态，并且加入到运行队列中
        p->state = RUNNABLE;
        _insert_into_list(&rq, &p->schinfo.rq);
    }else{
        PANIC();
    }
    release_sched_lock();
    return true;
}

static void update_this_state(enum procstate new_state)
{
    // TODO: if you use template sched function, you should implement this routinue
    // update the state of current process to new_state, and modify the sched queue if necessary
    if(debug_sched)printk("update_this_state\n");

    thisproc()->state = new_state;
    if(debug_sched)printk("update_this_state: new_state = %d\n", new_state);
    if(new_state == SLEEPING || new_state == ZOMBIE ){
        // if(debug_sched)printk("update_this_state: remove from rq\n");
        detach_from_list(&rqlock, &thisproc()->schinfo.rq);
        // if(debug_sched)printk("update_this_state: remove from rq done\n");
    }
}

static Proc *pick_next()
{
    // TODO: if using template sched function, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process
    if(debug_sched)printk("pick_next\n");
    acquire_spinlock(&rqlock);
    //便利运行队列，找到下一个可运行的进程
    _for_in_list(p, &rq){
        if(p == &rq)
            continue;
        
        auto proc = container_of(p, struct Proc, schinfo.rq);
        if(proc->state == RUNNABLE && !proc->idle){
            release_spinlock(&rqlock);
            if(debug_sched)printk("pick_next: pid = %d\n", proc->pid);
            return proc;
        }
    }
    //下一个lab可以设置一些更精妙的算法
    release_spinlock(&rqlock);
    return cpus[cpuid()].sched.idle;
}

static void update_this_proc(Proc *p)
{
    // TODO: you should implement this routinue
    // update thisproc to the choosen process

    if(debug_sched)printk("update_this_proc\n");
    // timer_init(1000);
    acquire_spinlock(&rqlock);
    cpus[cpuid()].sched.this_proc = p;  
    release_spinlock(&rqlock);
    if(debug_sched)printk("update_this_proc: pid = %d\n", p->pid);
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
// call with sched_lock
void sched(enum procstate new_state)
{
    if(debug_sched)printk("sched\n");
    auto this = thisproc();
    if(this->state == ZOMBIE){
        //防止因为并发导致 父进程wait时，子进程sched未被执行
        this->state = RUNNING;
    }
    ASSERT(this->state == RUNNING);
    update_this_state(new_state);
    auto next = pick_next();
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    //如果下一个进程不是当前进程，则切换上下文；（可能是idle进程）
    if (next->pid != this->pid) {
        if (debug_sched) {
            printk("switch %d -> %d\n", this->pid, next->pid);
        }

        swtch(next->kcontext, &this->kcontext);
        if(debug_sched)printk("swtch done\n");
    }
    release_sched_lock();
    if(debug_sched)printk("sched done\n");
}

u64 proc_entry(void (*entry)(u64), u64 arg)
{   
    if(debug_sched)printk("proc_entry\n");
    release_sched_lock();
    set_return_addr(entry);
    return arg;
}
