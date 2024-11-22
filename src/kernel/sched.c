#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <common/rbtree.h>


#define TIME_ELAPSE 2

extern bool panic_flag;
int debug_sched = 0;
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

// bool _activate_proc(Proc *p, bool onalert)
bool activate_proc(Proc *p)
{
    if(debug_sched)printk("activate_proc on CPU %lld\n", cpuid());
    acquire_sched_lock();
    if(debug_sched)printk("activate_proc on CPU %lld: pid = %d,state(old):%d\n", cpuid(), p->pid,p->state);
    // if the proc->state is DEEPSLEEPING, do nothing if onalert or activate it if else, and return the corresponding value.

    // if the proc->state is RUNNING/RUNNABLE, do nothing and return false
    // if (p->state == RUNNING || p->state == RUNNABLE || (p->state == DEEPSLEEPING && onalert)) {
    if (p->state == RUNNING || p->state == RUNNABLE ) {

        release_sched_lock();
        return false;
    }

    // if the proc->state is SLEEPING/UNUSED, set the process state to RUNNABLE, add it to the sched queue, and return true
    // if (p->state == SLEEPING || p->state == UNUSED || (p->state == DEEPSLEEPING && !onalert)) {
        if (p->state == SLEEPING || p->state == UNUSED) {
        p->state = RUNNABLE;
        
        insert_into_list_lockfree(sched_list.prev, &p->schinfo.node);
        if(debug_sched)printk("activate_proc on CPU %lld: pid = %d,state(new):%d\n", cpuid(), p->pid,p->state);
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
    if(debug_sched)printk("pick_next(pick_next) on CPU %lld: pid = %d,state:%d\n", cpuid(), proc->pid,proc->state);
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
    if(debug_sched)printk("update_this_state on CPU %lld: pid = %d,state:%d\n", cpuid(), this->pid,this->state);

    auto next = pick_next();
    if(debug_sched)printk("pick_next on CPU %lld: pid = %d,state:%d\n", cpuid(), next->pid,next->state);
    
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