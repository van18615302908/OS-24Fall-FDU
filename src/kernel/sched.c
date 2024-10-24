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

struct timer sched_timer[4];//每个CPU维护一个定时器

static void sched_handler(struct timer *timer)
{
    if(debug_sched)printk("sched_handler on CPU:%lld\n", cpuid());
    if(!panic_flag){
        acquire_sched_lock();
        timer->triggered = false;
        sched(RUNNABLE);
    }
}



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
        p->pid = -1;
        p->idle = true;
        p->killed = false;
        p->state = RUNNING;
        cpus[i].sched.this_proc = cpus[i].sched.idle = p;
        sched_timer[i].triggered = false;
        sched_timer[i].elapse = 10;
        sched_timer[i].handler = sched_handler;
    }

}

Proc *thisproc()
{

    // TODO: return the current process
    return cpus[cpuid()].sched.this_proc;
}

void init_schinfo(struct schinfo *p)
{
    if(debug_sched)printk("init_schinfo on CPU:%lld\n", cpuid());
    // TODO: initialize your customized schinfo for every newly-created process
    init_list_node(&p->rq);
}

void acquire_sched_lock()
{

    // TODO: acquire the sched_lock if need
    acquire_spinlock(&sched_lock);
}

void release_sched_lock()
{

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
    if(debug_sched)printk("activate_proc on CPU:%lld\n", cpuid());
    acquire_sched_lock();
    if(p->state == RUNNING || p->state == RUNNABLE){
        release_sched_lock();
        return false;
    }else if(p->state == SLEEPING || p->state == UNUSED){
        p->state = RUNNABLE;
        _insert_into_list(&rq, &p->schinfo.rq);
        // insert_at_tail(&rq, &p->schinfo.rq);
    }else{
        // PANIC();
        return false;
    }
    release_sched_lock();
    return true;
}

bool is_unused(struct Proc* p)
{
    bool r;
    acquire_sched_lock();
    r = p->state == UNUSED;
    release_sched_lock();
    return r;
}

static void update_this_state(enum procstate new_state)
{
    // TODO: if you use template sched function, you should implement this routinue
    // update the state of current process to new_state, and modify the sched queue if necessary
    if(debug_sched)printk("update_this_state pid:%d (old) on cpu:%lld\n", thisproc()->pid,cpuid());
    // printk("update_this_state pid:%d (old) to state:%d on cpu:%lld\n", thisproc()->pid,new_state,cpuid());
    thisproc()->state = new_state;
    if(debug_sched)printk("update_this_state pid:%d on CPU:%lld new_state = %d\n", thisproc()->pid,cpuid(),new_state);
    if(new_state == SLEEPING || new_state == ZOMBIE ){
        detach_from_list(&rqlock, &thisproc()->schinfo.rq);
        // printk("detach_from_list on CPU%lld: pid = %d\n", cpuid(),thisproc()->pid);
    }
}

static Proc *pick_next()
{
    // TODO: if using template sched function, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process
    acquire_spinlock(&rqlock);
    //便利运行队列，找到下一个可运行的进程
    _for_in_list(p, &rq){
        if(p == &rq || p == &thisproc()->schinfo.rq)
            continue;
        
        auto proc = container_of(p, struct Proc, schinfo.rq);
        if(proc->state == RUNNABLE && proc->pid > -1){
            release_spinlock(&rqlock);
            if(debug_sched)printk("pick_next on CPU%lld: pid = %d\n", cpuid(),proc->pid);
            return proc;
        }
    }
    //下一个lab可以设置一些更精妙的算法
    release_spinlock(&rqlock);
    if(debug_sched) printk("（pick_next）No runnable process on CPU%lld\n", cpuid());
    // printk("（pick_next）No runnable process on CPU%lld\n", cpuid());
    return cpus[cpuid()].sched.idle;
}

static void update_this_proc(Proc *p)
{
    // TODO: you should implement this routinue
    // update thisproc to the choosen process

    if(debug_sched)printk("update_this_proc(old) on CPU%lld :pid = %d\n",cpuid(), thisproc()->pid);
    // timer_init(1000);
    acquire_spinlock(&rqlock);
    cpus[cpuid()].sched.this_proc = p;  
    release_spinlock(&rqlock);
    auto timer = &sched_timer[cpuid()];
    if(!timer->triggered){
        cancel_cpu_timer(timer);
    }
    set_cpu_timer(timer);

}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
// call with sched_lock
void sched(enum procstate new_state)
{
    if(debug_sched)printk("sched  on CPU %lld\n", cpuid());
    auto this = thisproc();
    if(this->state == ZOMBIE){
        //防止因为并发导致 父进程wait时，子进程sched未被执行
        this->state = RUNNING;
    }
    ASSERT(this->state == RUNNING);
    if(debug_sched)printk("(shed)thisproc on CPU %lld:pid = %d\n",cpuid(), this->pid);
    if (debug_sched) {
        printk("Current CPU %lld processes:\n", cpuid());
        _for_in_list(p, &rq) {
            if (p == &rq)
                continue;
            auto proc = container_of(p, struct Proc, schinfo.rq);
            printk("PID: %d, State: %d\n", proc->pid, proc->state);
        }
    }
    //首次sched的时候，可能也符合条件 因此加上对pid的单独判断
    if(this->killed && new_state != ZOMBIE && this->pid > 0){
        if(debug_sched)printk("sched on CPU %lld: done\n", cpuid());
        release_sched_lock();
        return;
    }
    update_this_state(new_state);
    if(new_state == RUNNABLE && this->pid > -1){//idle进程不加入队列
        detach_from_list(&rqlock, &thisproc()->schinfo.rq);
        insert_at_tail(&rq, &thisproc()->schinfo.rq);
        // printk("insert_at_tail on CPU%lld: pid = %d\n", cpuid(),thisproc()->pid);
    }
    auto next = pick_next();
    if(debug_sched)printk("pick_next on CPU %lld: pid = %d\n", cpuid(),next->pid);
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    //如果下一个进程不是当前进程，则切换上下文；（可能是idle进程）
    if (next->pid != this->pid) {
        if (debug_sched) {
            printk("switch on CPU %lld: %d -> %d\n",cpuid(), this->pid, next->pid);
        }
        attach_pgdir(&next->pgdir);
        swtch(next->kcontext, &this->kcontext);
        if(debug_sched)printk("swtch done on CPU %lld\n", cpuid());
    }
    release_sched_lock();
    if(debug_sched)printk("sched done on CPU %lld\n", cpuid());
}

u64 proc_entry(void (*entry)(u64), u64 arg)
{   
    if(debug_sched)printk("proc_entry on CPU %lld\n", cpuid());
    release_sched_lock();
    set_return_addr(entry);
    return arg;
}
