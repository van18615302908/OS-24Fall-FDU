#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <kernel/pid.h>

Proc root_proc;
int debug_proc = 0;

static SpinLock global_process_lock;
void kernel_entry();
void proc_entry();

// init_kproc initializes the kernel process
// NOTE: should call after kinit
void init_kproc()
{
    if(debug_proc)printk("init_kproc on CPU %lld\n",cpuid());

    init_spinlock(&global_process_lock);
    init_pid_pool();

    init_proc(&root_proc);
    root_proc.parent = &root_proc;

    start_proc(&root_proc, kernel_entry, 123456);
}

void init_proc(Proc *p)
{
    if(debug_proc)printk("init_proc on CPU %lld\n",cpuid());
    acquire_spinlock(&global_process_lock);
    memset(p, 0, sizeof(Proc));

    p->pid = allocate_pid();
    if(debug_proc)printk("init_proc pid:%d\n", p->pid);
    p->state = UNUSED;
    if(debug_proc)printk("init_proc pid %d state:%d\n", p->pid,p->state);
    p->kstack = kalloc_page();
    p->ucontext = (UserContext *)((u64)p->kstack + PAGE_SIZE - sizeof(KernelContext) - sizeof(UserContext));
    p->kcontext = (KernelContext *)((u64)p->ucontext - sizeof(KernelContext));

    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    init_list_node(&p->zombie_children);

    init_sem(&p->childexit, 0);
    init_schinfo(&p->schinfo);
    init_pgdir(&p->pgdir);
    release_spinlock(&global_process_lock);
}

Proc *create_proc()
{
    if(debug_proc)printk("create_proc on CPU %lld\n",cpuid());
    Proc *p = kalloc(sizeof(Proc));
    init_proc(p);
    return p;
}

void set_parent_to_this(Proc *proc)
{
    if(debug_proc)printk("set_parent_to_this\n");
    acquire_spinlock(&global_process_lock);

    ASSERT(proc->parent == NULL);

    Proc *current = thisproc();
    proc->parent = current;

    _detach_from_list(&proc->ptnode);
    _insert_into_list(&current->children, &proc->ptnode);

    release_spinlock(&global_process_lock);

}

int start_proc(Proc *p, void (*entry)(u64), u64 arg)
{
    if(debug_proc)printk("start_proc on CPU %lld\n",cpuid());
    acquire_spinlock(&global_process_lock);

    if (p->parent == NULL) {
        p->parent = &root_proc;
        _detach_from_list(&p->ptnode);
        _insert_into_list(&root_proc.children, &p->ptnode);
    }

    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = arg;

    int pid = p->pid;

    activate_proc(p);
    release_spinlock(&global_process_lock);

    return pid;
}

int wait(int *exitcode)
{
    if(debug_proc)printk("wait on CPU %lld\n",cpuid());
    acquire_spinlock(&global_process_lock);

    Proc *current = thisproc();

    // 1. return -1 if no children
    if (_empty_list(&current->children) &&
        _empty_list(&current->zombie_children)) {
        ASSERT(current->children.prev == &current->children);
        ASSERT(current->zombie_children.prev == &current->zombie_children);

        release_spinlock(&global_process_lock);

        return -1;
    }

    // 2. wait for childexit
    release_spinlock(&global_process_lock);

    bool r = wait_sem(&current->childexit);
    ASSERT(r);

    // 3. if any child exits, clean it up （kstack, ..., use kfree）and return its pid and exitcode
    acquire_spinlock(&global_process_lock);
    ListNode *p = current->zombie_children.next;

    ASSERT(p != &current->zombie_children);
    Proc *proc = container_of(p, Proc, ptnode);
    *exitcode = proc->exitcode;
    _detach_from_list(p);
    int pid = proc->pid;

    free_pid(proc->pid);
    // free_pidmap(proc->pid, &pidmap, &h);
    kfree_page(proc->kstack);
    free_pgdir(&proc->pgdir);
    kfree(proc);
    release_spinlock(&global_process_lock);
    return pid;
}

NO_RETURN void exit(int code)
{
    if(debug_proc)printk("exit on CPU %lld\n",cpuid());
    acquire_spinlock(&global_process_lock);
    Proc *current = thisproc();
    // 1. set the exitcode
    current->exitcode = code;

    // 2. clean up the resources

    // 3. transfer children to the root_proc, and notify the root_proc if there is zombie
    _for_in_list(p, &current->children)
    {
        if (p == &current->children) {
            continue;
        }
        Proc *proc = container_of(p, Proc, ptnode);
        proc->parent = &root_proc;
    }

    if (!_empty_list(&current->children)) {
        // PANIC();
        insert_list_into_list(current->children.next,
                                       current->children.prev,
                                       &root_proc.children);
    }

    for (ListNode *p = current->zombie_children.next,
                  *p_next = current->zombie_children.next->next;
         p != &current->zombie_children; p = p_next) {
        Proc *proc = container_of(p, Proc, ptnode);
        proc->parent = &root_proc;
        p_next = p->next;
        _detach_from_list(p);
        _insert_into_list(&root_proc.zombie_children, p);
        post_sem(&root_proc.childexit);
    }

    // Remove self from parent's child list and add to zombie child list
    _detach_from_list(&current->ptnode);
    _insert_into_list(&current->parent->zombie_children,
                              &current->ptnode);

    // Notify parent
    post_sem(&current->parent->childexit);
    acquire_sched_lock();
    release_spinlock(&global_process_lock);

    sched(ZOMBIE);
    PANIC(); // prevent the warning of 'no_return function returns'
}

Proc *find_proc_by_pid(Proc *start_proc, int pid)
{
    _for_in_list(p, &start_proc->children)
    {
        if (p == &start_proc->children) {
            continue;
        }
        Proc *proc = container_of(p, Proc, ptnode);
        if (proc->pid == pid) {
            ASSERT(!is_unused(proc));
            return proc;
        } else {
            Proc *p = find_proc_by_pid(proc, pid);
            if (p) {
                return p;
            }
        }
    }
    return NULL;
}

int kill(int pid)
{
    if(debug_proc)printk("kill on CPU %lld ,pid:%d\n",cpuid(),pid);
    // Set the killed flag of the proc to true and return 0.
    acquire_spinlock(&global_process_lock);

    Proc *proc = find_proc_by_pid(&root_proc, pid);
    if (proc) {
        proc->killed = true;
        // alert_proc(proc);
        activate_proc(proc);
        release_spinlock(&global_process_lock);

        return 0;
    }
    // Return -1 if the pid is invalid (proc not found).
    release_spinlock(&global_process_lock);

    return -1;
}