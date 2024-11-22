// #include <kernel/sched.h>
// #include <kernel/proc.h>
// #include <kernel/mem.h>
// #include <kernel/printk.h>
// #include <aarch64/intrinsic.h>
// #include <kernel/cpu.h>
// #include <common/rbtree.h>
// #include <driver/timer.h>

// extern bool panic_flag;

// extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);
// int debug_sched = 0;

// static SpinLock sched_lock;
// static ListNode rq;
// static SpinLock rqlock;

// struct timer sched_timer[4];//每个CPU维护一个定时器

// static void sched_handler(struct timer *timer)
// {
//     if(debug_sched)printk("sched_handler on CPU:%lld\n", cpuid());
//     if(!panic_flag){
//         acquire_sched_lock();
//         timer->triggered = false;
//         sched(RUNNABLE);
//     }
// }



// void init_sched()
// {
//     // TODO: initialize the scheduler
//     // 1. initialize the resources (e.g. locks, semaphores)
//     // 2. initialize the scheduler info of each CPU
//     if(debug_sched)printk("init_sched\n");
//     init_spinlock(&sched_lock);
//     init_list_node(&rq);
//     for(int i=0; i<NCPU; i++){
//         struct Proc* p = kalloc(sizeof(struct Proc));
//         p->pid = -1;
//         p->idle = true;
//         p->killed = false;
//         p->state = RUNNING;
//         cpus[i].sched.this_proc = cpus[i].sched.idle = p;
//         sched_timer[i].triggered = false;
//         sched_timer[i].elapse = 10;
//         sched_timer[i].handler = sched_handler;
//     }

// }

// Proc *thisproc()
// {

//     // TODO: return the current process
//     return cpus[cpuid()].sched.this_proc;
// }

// void init_schinfo(struct schinfo *p)
// {
//     if(debug_sched)printk("init_schinfo on CPU:%lld\n", cpuid());
//     // TODO: initialize your customized schinfo for every newly-created process
//     init_list_node(&p->rq);
// }

// void acquire_sched_lock()
// {

//     // TODO: acquire the sched_lock if need
//     acquire_spinlock(&sched_lock);
// }

// void release_sched_lock()
// {

//     // TODO: release the sched_lock if need
//     release_spinlock(&sched_lock);
// }

// bool is_zombie(Proc *p)
// {
//     bool r;
//     r = p->state == ZOMBIE;
//     return r;
// }


// bool activate_proc(Proc *p)
// {
//     // TODO:
//     // if the proc->state is RUNNING/RUNNABLE, do nothing
//     // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
//     // else: panic
//     if(debug_sched)printk("activate_proc on CPU:%lld\n", cpuid());
//     acquire_sched_lock();
//     if(p->state == RUNNING || p->state == RUNNABLE){
//         release_sched_lock();
//         return false;
//     }else if(p->state == SLEEPING || p->state == UNUSED){
//         p->state = RUNNABLE;
//         _insert_into_list(&rq, &p->schinfo.rq);
//         // insert_at_tail(&rq, &p->schinfo.rq);
//     }else{
//         // PANIC();
//         return false;
//     }
//     release_sched_lock();
//     return true;
// }

// bool is_unused(struct Proc* p)
// {
//     bool r;
//     acquire_sched_lock();
//     r = p->state == UNUSED;
//     release_sched_lock();
//     return r;
// }

// static void update_this_state(enum procstate new_state)
// {
//     // TODO: if you use template sched function, you should implement this routinue
//     // update the state of current process to new_state, and modify the sched queue if necessary
//     if(debug_sched)printk("update_this_state pid:%d (old) on cpu:%lld\n", thisproc()->pid,cpuid());
//     // printk("update_this_state pid:%d (old) to state:%d on cpu:%lld\n", thisproc()->pid,new_state,cpuid());
//     thisproc()->state = new_state;
//     if(debug_sched)printk("update_this_state pid:%d on CPU:%lld new_state = %d\n", thisproc()->pid,cpuid(),new_state);
//     if(new_state == SLEEPING || new_state == ZOMBIE ){
//         if(thisproc()->pid  <0){
//             // detach_from_list(&rqlock, &thisproc()->schinfo.rq);
//             printk("detach_from_list on CPU%lld: pid = %d\n", cpuid(),thisproc()->pid);
//         }
//         detach_from_list(&rqlock, &thisproc()->schinfo.rq);
//         // printk("detach_from_list on CPU%lld: pid = %d\n", cpuid(),thisproc()->pid);
//     }
// }

// static Proc *pick_next()
// {
//     // TODO: if using template sched function, you should implement this routinue
//     // choose the next process to run, and return idle if no runnable process
//     acquire_spinlock(&rqlock);
//     //便利运行队列，找到下一个可运行的进程
//     _for_in_list(p, &rq){
//         if(p == &rq || p == &thisproc()->schinfo.rq)
//             continue;
        
//         auto proc = container_of(p, struct Proc, schinfo.rq);
//         if(proc->state == RUNNABLE && proc->pid > -1){
//             release_spinlock(&rqlock);
//             if(debug_sched)printk("pick_next on CPU%lld: pid = %d\n", cpuid(),proc->pid);
//             return proc;
//         }
//     }
//     //下一个lab可以设置一些更精妙的算法
//     release_spinlock(&rqlock);
//     if(debug_sched) printk("（pick_next）No runnable process on CPU%lld\n", cpuid());
//     // printk("（pick_next）No runnable process on CPU%lld\n", cpuid());
//     return cpus[cpuid()].sched.idle;
// }

// static void update_this_proc(Proc *p)
// {
//     // TODO: you should implement this routinue
//     // update thisproc to the choosen process

//     if(debug_sched)printk("update_this_proc(old) on CPU%lld :pid = %d\n",cpuid(), thisproc()->pid);
//     // timer_init(1000);
//     acquire_spinlock(&rqlock);
//     cpus[cpuid()].sched.this_proc = p;  
//     release_spinlock(&rqlock);
//     auto timer = &sched_timer[cpuid()];
//     if(!timer->triggered){
//         cancel_cpu_timer(timer);
//     }
//     set_cpu_timer(timer);

// }


// // You are allowed to replace it with whatever you like.
// // call with sched_lock
// void sched(enum procstate new_state)
// {
//     if(debug_sched)printk("sched  on CPU %lld\n", cpuid());
//     auto this = thisproc();
//     if(this->state == ZOMBIE){
//         //防止因为并发导致 父进程wait时，子进程sched未被执行
//         this->state = RUNNING;
//     }
//     ASSERT(this->state == RUNNING);
//     if(debug_sched)printk("(shed)thisproc on CPU %lld:pid = %d\n",cpuid(), this->pid);
//     if (debug_sched) {
//         printk("Current CPU %lld processes:\n", cpuid());
//         _for_in_list(p, &rq) {
//             if (p == &rq)
//                 continue;
//             auto proc = container_of(p, struct Proc, schinfo.rq);
//             printk("PID: %d, State: %d\n", proc->pid, proc->state);
//         }
//     }
//     //首次sched的时候，可能也符合条件 因此加上对pid的单独判断
//     if(this->killed && new_state != ZOMBIE && this->pid > 0){
//         if(debug_sched)printk("sched on CPU %lld: done\n", cpuid());
//         release_sched_lock();
//         return;
//     }
//     update_this_state(new_state);
//     if(new_state == RUNNABLE && this->pid > -1){//idle进程不加入队列
//         detach_from_list(&rqlock, &thisproc()->schinfo.rq);
//         insert_at_tail(&rq, &thisproc()->schinfo.rq);
//         // printk("insert_at_tail on CPU%lld: pid = %d\n", cpuid(),thisproc()->pid);
//     }
//     auto next = pick_next();
//     if(debug_sched)printk("pick_next on CPU %lld: pid = %d\n", cpuid(),next->pid);
//     update_this_proc(next);
//     ASSERT(next->state == RUNNABLE);
//     next->state = RUNNING;
//     //如果下一个进程不是当前进程，则切换上下文；（可能是idle进程）
//     if (next->pid != this->pid) {
//         if (debug_sched) {
//             printk("switch on CPU %lld: %d -> %d\n",cpuid(), this->pid, next->pid);
//         }
//         attach_pgdir(&next->pgdir);
//         swtch(next->kcontext, &this->kcontext);
//         if(debug_sched)printk("swtch done on CPU %lld\n", cpuid());
//     }
//     release_sched_lock();
//     if(debug_sched)printk("sched done on CPU %lld\n", cpuid());
// }

// u64 proc_entry(void (*entry)(u64), u64 arg)
// {   
//     if(debug_sched)printk("proc_entry on CPU %lld\n", cpuid());
//     release_sched_lock();
//     set_return_addr(entry);
//     return arg;
// }
#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <common/rbtree.h>


#define TIME_ELAPSE 2

extern bool panic_flag;
int debug_sched = 1;
extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);

static struct timer sched_timer[NCPU];
static SpinLock sched_list_lock;
static ListNode sched_list;

static void _sched_timer_handler(struct timer *t)
{
    yield();
}

void init_sched()
{
    if(debug_sched)printk("init_sched on CPI %lld\n", cpuid());
    // 1. initialize the resources (e.g. locks, semaphores)
    init_list_node(&sched_list);
    init_spinlock(&sched_list_lock);

    // 2. initialize the scheduler info of each CPU
    for (int i = 0; i < NCPU; i++) {
        Proc *p = (Proc *)kalloc(sizeof(Proc));
        p->idle = true;
        p->parent = p;
        p->killed = false;
        p->pid = -1;
        p->state = RUNNING;
        cpus[i].sched.idle = p;
        cpus[i].sched.current = p;
        sched_timer[i].elapse = TIME_ELAPSE;
        sched_timer[i].handler = _sched_timer_handler;
        sched_timer[i].triggered = true;
    }
}

Proc *thisproc()
{
    return cpus[cpuid()].sched.current;
}

void init_schinfo(struct schinfo *p)
{
    init_list_node(&p->node);
}

void acquire_sched_lock()
{
    acquire_spinlock(&sched_list_lock);
}

void release_sched_lock()
{
    release_spinlock(&sched_list_lock);
}

void assert_sched_locked()
{
    ASSERT(sched_list_lock.locked);
}

bool is_zombie(Proc *p)
{
    bool r;
    acquire_sched_lock();
    r = p->state == ZOMBIE;
    release_sched_lock();
    return r;
}

bool is_unused(Proc *p)
{
    bool r;
    acquire_sched_lock();
    r = p->state == UNUSED;
    release_sched_lock();
    return r;
}

bool _activate_proc(Proc *p, bool onalert)
// bool activate_proc(Proc *p)
{
    if(debug_sched)printk("activate_proc on CPU %lld\n", cpuid());
    acquire_sched_lock();
    printk("activate_proc on CPU %lld: pid = %d,state(old):%d\n", cpuid(), p->pid,p->state);
    // if the proc->state is DEEPSLEEPING, do nothing if onalert or activate it if else, and return the corresponding value.

    // if the proc->state is RUNNING/RUNNABLE, do nothing and return false
    if (p->state == RUNNING || p->state == RUNNABLE || (p->state == DEEPSLEEPING && onalert)) {
    // if (p->state == RUNNING || p->state == RUNNABLE ) {

        release_sched_lock();
        return false;
    }

    // if the proc->state is SLEEPING/UNUSED, set the process state to RUNNABLE, add it to the sched queue, and return true
    if (p->state == SLEEPING || p->state == UNUSED || (p->state == DEEPSLEEPING && !onalert)) {
        // if (p->state == SLEEPING || p->state == UNUSED) {
        p->state = RUNNABLE;
        
        insert_into_list_lockfree(sched_list.prev, &p->schinfo.node);
        printk("activate_proc on CPU %lld: pid = %d,state(new):%d\n", cpuid(), p->pid,p->state);
        release_sched_lock();
        return true;
    }

    release_sched_lock();
    return false;
}

// This function is called by `sched`, which has already been protected by lock.
void update_this_state(enum procstate new_state)
{
    // update the state of current process to new_state, and modify the sched queue if necessary
    if(debug_sched)printk("update_this_state on CPU %lld\n", cpuid());
    assert_sched_locked();
    Proc *current = thisproc();
    enum procstate old_state = current->state;
    current->state = new_state;
    if (!current->idle) {
        if (new_state == RUNNABLE) {
            insert_into_list_lockfree(sched_list.prev, &current->schinfo.node);
        } else if (old_state == RUNNABLE) {
            _for_in_list(p, &sched_list)
            {
                if (p == &sched_list) {
                    continue;
                }
                Proc *proc = container_of((struct schinfo *)p, Proc, schinfo);
                if (proc->pid == current->pid) {
                    _detach_from_list(p);
                    break;
                }
            }
        }
    }
}

// This function is called by `sched`, which has already been protected by lock.
static Proc *pick_next()
{
    if(debug_sched)printk("pick_next on CPU %lld\n", cpuid());
    // choose the next process to run, and return idle if no runnable process
    if (panic_flag) {
        return cpus[cpuid()].sched.idle;
    }

    if (_empty_list(&sched_list)) {

        return cpus[cpuid()].sched.idle;
    }

    ListNode *p = sched_list.next;
    Proc *proc = container_of((struct schinfo *)p, Proc, schinfo);
    printk("pick_next(pick_next) on CPU %lld: pid = %d,state:%d\n", cpuid(), proc->pid,proc->state);
    _detach_from_list(p);
    return proc;
}

// This function is called by `sched`, which has already been protected by lock.
static void update_this_proc(Proc *p)
{
    if(debug_sched)printk("update_this_proc on CPU %lld\n", cpuid());
// update thisproc to the choosen process

    if (!sched_timer[cpuid()].triggered) {
        cancel_cpu_timer(&sched_timer[cpuid()]);
    }
    cpus[cpuid()].sched.current = p;
    sched_timer[cpuid()].elapse = TIME_ELAPSE;
    sched_timer[cpuid()].handler = _sched_timer_handler;
    set_cpu_timer(&sched_timer[cpuid()]);
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
// call with sched_lock
void sched(enum procstate new_state)
{
    if(debug_sched)printk("sched on CPU %lld\n", cpuid());
    assert_sched_locked();

    auto this = thisproc();
    ASSERT(this->state == RUNNING);

    if (this->killed && new_state != ZOMBIE) {
        release_sched_lock();
        return;
    }

    update_this_state(new_state);
    printk("update_this_state on CPU %lld: pid = %d,state:%d\n", cpuid(), this->pid,this->state);

    auto next = pick_next();
    printk("pick_next on CPU %lld: pid = %d,state:%d\n", cpuid(), next->pid,next->state);
    
    ASSERT(next->state == RUNNABLE);
    update_this_proc(next);
    next->state = RUNNING;

    if (next != this) {
        attach_pgdir(&next->pgdir);
        swtch(next->kcontext, &this->kcontext);
    }
    release_sched_lock();
}

u64 proc_entry(void (*entry)(u64), u64 arg)
{
    release_sched_lock();
    set_return_addr(entry);
    return arg;
}