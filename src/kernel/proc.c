#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <kernel/paging.h>
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

/*
 * Create a new process copying p as the parent.
 * Sets up stack to return as if from system call.
 */
void trap_return();
int fork()
{
    /**
     * (Final) TODO BEGIN
     * 
     * 1. Create a new child process.
     * 2. Copy the parent's memory space.
     * 3. Copy the parent's trapframe.
     * 4. Set the parent of the new proc to the parent of the parent.
     * 5. Set the state of the new proc to RUNNABLE.
     * 6. Activate the new proc and return its pid.
     */

    /* (Final) TODO END */
    auto this = thisproc();
    auto new = create_proc();
    //设置父子关系
    acquire_spinlock(&global_process_lock);
    new->parent = this;
    _insert_into_list(&this->children, &new->ptnode);
    release_spinlock(&global_process_lock);
    // TrapFrame 和用户上下文
    memcpy((void*)new->ucontext, (void*)this->ucontext, sizeof(UserContext));
    new->ucontext->x[0] = 0;

    //遍历父进程的页面段表，为子进程分配相应的内存段
    acquire_spinlock(&this->pgdir.lock);
    _for_in_list(p, &this->pgdir.section_head){
        if(p != &this->pgdir.section_head){
            auto st = container_of(p, struct section, stnode);
            auto new_st = (struct section*)kalloc(sizeof(struct section));
            memset(new_st, 0, sizeof(struct section));
            if(new_st == NULL){
                ASSERT(kill(new->pid) != -1);
                break;
            }
            new_st->begin = st->begin;
            new_st->end = st->end;
            new_st->flags = st->flags;
            if(st->fp){
                new_st->fp = file_dup(st->fp);
                new_st->offset = st->offset;
                new_st->length = st->length;
            }
            _insert_into_list(new->pgdir.section_head.prev, &new_st->stnode);

            for(auto va = PAGE_BASE(st->begin); va < st->end; va += PAGE_SIZE){
                auto pte = get_pte(&this->pgdir, va, false);
                if(pte && (*pte & PTE_VALID)){
                    *pte |= PTE_RO;
                    vmmap(&new->pgdir, va, (void*)P2K(PTE_ADDRESS(*pte)), PTE_FLAGS(*pte));
                    kshare_page(P2K(PTE_ADDRESS(*pte)));
                    // copyout(&new->pgdir, (void*)va, (void*)P2K(PTE_ADDRESS(*pte)), PAGE_SIZE);
                    // auto new_pte = get_pte(&new->pgdir, va, false);
                    // *new_pte |= PTE_USER_DATA | PTE_RW;
                }
            }
        }
    }
    arch_tlbi_vmalle1is();//刷新TLB
    release_spinlock(&this->pgdir.lock);
    //复制当前进程的工作目录（cwd）到子进程。如果工作目录不同于父进程，则需要增加引用计数。
    memset((void*)&new->oftable, 0, sizeof(struct oftable));
    if(new->cwd != this->cwd){
        OpContext ctx;
        bcache.begin_op(&ctx);
        inodes.put(&ctx, new->cwd);
        bcache.end_op(&ctx);
        new->cwd = inodes.share(this->cwd);
    }

    for(auto i = 0; i < NOFILE; i++){
        if(this->oftable.fp[i] ){//may_bug
            new->oftable.fp[i] = file_dup(this->oftable.fp[i]);
        }
        else break;
    }

    start_proc(new, trap_return, 0);

    return new->pid;
}