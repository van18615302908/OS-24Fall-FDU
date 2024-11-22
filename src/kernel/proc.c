// #include <kernel/proc.h>
// #include <kernel/mem.h>
// #include <kernel/sched.h>
// #include <aarch64/mmu.h>
// #include <common/list.h>
// #include <common/string.h>
// #include <kernel/printk.h>
// #include <kernel/pid.h>

// Proc root_proc;

// void kernel_entry();
// void proc_entry();

// // 定义全局锁
// static SpinLock global_lock;

// int debug_fyy = 0;

// static hash_map h;
// pidmap_t pidmap = { PID_MAX_DEFAULT, {0}};
// int last_pid = -1;




// // init_kproc initializes the kernel process
// // NOTE: should call after kinit
// void init_kproc()
// {
//     // TODO:
//     // 1. init global resources (e.g. locks, semaphores)
//     // 2. init the root_proc (finished)
//     if(debug_fyy)printk("init_kproc on CPU %lld\n",cpuid());
//     h = kalloc(sizeof(struct hash_map_));
//     hashmap_init(h);
//     // 初始化全局锁
//     init_spinlock(&global_lock);
//     init_proc(&root_proc);
//     root_proc.parent = &root_proc;
//     start_proc(&root_proc, kernel_entry, 123456);
// }

// void init_proc(Proc *p)
// {
//     // TODO:
//     // setup the Proc with kstack and pid allocated
//     // NOTE: be careful of concurrency
//     if(debug_fyy)printk("init_proc on CPU %lld\n",cpuid());
//     p->killed = false;
//     p->idle = false;
//     acquire_spinlock(&global_lock);
//     p->pid = alloc_pidmap(p, &last_pid, &pidmap, &h);
//     release_spinlock(&global_lock);
//     p->state = UNUSED;
//     init_sem(&(p->childexit),0);
//     init_list_node(&(p->children));
//     init_list_node(&(p->ptnode));
//     p->parent = NULL;
//     init_schinfo(&(p->schinfo));
//     p->kstack = kalloc_page();
//     p->ucontext = p->kstack + PAGE_SIZE - 16 - sizeof(UserContext);
//     p->kcontext = p->kstack + PAGE_SIZE - 16 - sizeof(UserContext) - sizeof(KernelContext);
//     init_pgdir(&p->pgdir);
// }

// Proc *create_proc()
// {
//     if(debug_fyy)printk("create_proc on CPU %lld\n",cpuid());
//     Proc *p = kalloc(sizeof(Proc));
//     init_proc(p);
//     return p;
// }

// void set_parent_to_this(Proc *proc)
// {
//     // TODO: set the parent of proc to thisproc
//     // NOTE: maybe you need to lock the process tree
//     // NOTE: it's ensured that the old proc->parent = NULL
//     if(debug_fyy)printk("set_parent_to_this\n");
//     ASSERT(proc->parent == NULL);
//     acquire_spinlock(&global_lock);
//     proc->parent = thisproc();
//     _insert_into_list(&thisproc()->children, &proc->ptnode);
//     release_spinlock(&global_lock);
// }

// int start_proc(Proc *p, void (*entry)(u64), u64 arg)
// {
//     // TODO:
//     // 1. set the parent to root_proc if NULL
//     // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
//     // 3. activate the proc and return its pid
//     // NOTE: be careful of concurrency
//     if(debug_fyy)printk("start_proc on CPU %lld\n",cpuid());
//     if(p->parent == NULL){
//         acquire_spinlock(&global_lock);
//         p->parent = &root_proc;
//         _insert_into_list(&root_proc.children, &p->ptnode);
//         release_spinlock(&global_lock);
//     }
//     p->kcontext->lr = (u64)&proc_entry;
//     p->kcontext->x0 = (u64)entry;
//     p->kcontext->x1 = (u64)arg;
//     int id = p->pid;
//     activate_proc(p);
//     return id;
// }

// int wait(int *exitcode)
// {
//     // TODO:
//     // 1. return -1 if no children
//     // 2. wait for childexit
//     // 3. if any child exits, clean it up and return its pid and exitcode
//     // NOTE: be careful of concurrency
//     if(debug_fyy)printk("wait on CPU %lld\n",cpuid());

//     auto this = thisproc();
//     if(_empty_list(&this->children))
//         return -1;
//     //等待子进程退出导致的信号量的改变，此时父进程为SLEEPING状态，并且处于调度队列
//     //如果考虑并发，可能会导致父进程被其他进程调度，此时如果子进程还未退出，会导致父进程无法wait
//     wait_sem(&this->childexit);
//     acquire_spinlock(&global_lock);
//     //遍历子进程，找到第一个僵尸进程，将其从父进程的children队列中删除，并且释放资源
//     auto p = this->children.prev;
//     while(p != &this->children){
//         auto proc = container_of(p, struct Proc, ptnode);
//         if(is_zombie(proc)){
//             *exitcode = proc->exitcode;
//             int id = proc->pid;
//             _detach_from_list(p);
//             kfree_page(proc->kstack);
//             kfree(proc);
//             free_pidmap(id, &pidmap, &h);
//             release_spinlock(&global_lock);
//             return id;
//         }
//         p = p->prev;
//     }
//     printk("error in proc %d \n",this->state);
//     PANIC();
//     release_spinlock(&global_lock);
//     return -1;
// }

// NO_RETURN void exit(int code)
// {
//     // TODO:
//     // 1. set the exitcode
//     // 2. clean up the resources
//     // 3. transfer children to the root_proc, and notify the root_proc if there is zombie
//     // 4. sched(ZOMBIE)
//     // NOTE: be careful of concurrenc
//     //TODO clean up file resources
//     if(debug_fyy)printk("exit on CPU %lld\n",cpuid());
//     auto this = thisproc();
//     this->exitcode = code;
//     free_pgdir(&this->pgdir);

//     acquire_spinlock(&global_lock);
//     ListNode* pre = NULL;
//     //将子进程转移到root_proc
//     _for_in_list(p, &this->children){
//         if(pre != NULL && pre != &this->children){
//             auto proc = container_of(pre, struct Proc, ptnode);
//             proc->parent = &root_proc;
//             auto t = &root_proc.children;
//             //如果子进程是僵尸进程，直接插入到root_proc的children队列中，并且通知root_proc
//             if(is_zombie(proc)){
//                 pre->prev = t->prev;
//                 pre->next = t;
//                 t->prev->next = pre;
//                 t->prev = pre;
//                 post_sem(&root_proc.childexit);
//             }else{
//                 //如果子进程不是僵尸进程，直接插入到root_proc的children队列中
//                 _insert_into_list(t, pre);
//             }
//         }
//         pre = p;
//     }
//     // printk("2\n");
//     //将自己从父进程的children队列中删除
//     init_list_node(&this->children);
//     pre = &this->ptnode;
//     // printk("2.5 on CPU %lld\n",cpuid());
//     _detach_from_list(pre);
//     // printk("3 on CPU %lld\n",cpuid());
//     auto t = &this->parent->children;
//     pre->prev = t->prev;
//     pre->next = t;
//     t->prev->next = pre;
//     t->prev = pre;
//     this->state = ZOMBIE;//防止并发，导致被其他进程调度导致父进程无法wait
//     // printk("4\n");
//     //通知父进程
//     post_sem(&this->parent->childexit);
//     release_spinlock(&global_lock);
//     acquire_sched_lock();
//     //调度
//     sched(ZOMBIE);
//     PANIC(); // prevent the warning of 'no_return function returns'
// }

// int kill(int pid)
// {
//     printk("kill on CPU %lld ,pid:%d\n",cpuid(),pid);
    
//     // TODO:
//     // Set the killed flag of the proc to true and return 0.
//     // Return -1 if the pid is invalid (proc not found).
//     acquire_spinlock(&global_lock);
//     auto p = hashmap_lookup(&(hashpid_t){pid, NULL, {NULL}}.node, h, hash, hashcmp);
//     if(p != NULL){
//         auto proc = container_of(p, hashpid_t, node)->proc;
//         if(is_unused(proc)) return -1;
//         proc->killed = true;
//         activate_proc(proc);
//         release_spinlock(&global_lock);
//         return 0;
//     }
//     release_spinlock(&global_lock);
//     return -1;
// }
#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <kernel/pid.h>

Proc root_proc;
int debug_proc = 1;

static SpinLock global_process_lock;
void kernel_entry();
void proc_entry();

// init_kproc initializes the kernel process
// NOTE: should call after kinit
void init_kproc()
{
    if(debug_proc)printk("init_kproc on CPU %lld\n",cpuid());
    // 1. init global resources (e.g. locks, semaphores)
    init_spinlock(&global_process_lock);
    init_pid_pool();


    // 2. init the root_proc (finished)
    init_proc(&root_proc);
    root_proc.parent = &root_proc;

    start_proc(&root_proc, kernel_entry, 123456);
}

void init_proc(Proc *p)
{
    if(debug_proc)printk("init_proc on CPU %lld\n",cpuid());
    // setup the Proc with kstack and pid allocated
    acquire_spinlock(&global_process_lock);
    memset(p, 0, sizeof(Proc));

    p->pid = allocate_pid();
    printk("init_proc pid:%d\n", p->pid);
    p->state = UNUSED;
    printk("init_proc pid %d state:%d\n", p->pid,p->state);
    p->kstack = kalloc_page();
    p->ucontext = (UserContext *)((u64)p->kstack + PAGE_SIZE -
                                  sizeof(KernelContext) - sizeof(UserContext));
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
    // set the parent of proc to thisproc
    acquire_spinlock(&global_process_lock);

    ASSERT(proc->parent == NULL);

    Proc *current = thisproc();
    proc->parent = current;

    _detach_from_list(&proc->ptnode);
    insert_into_list_lockfree(&current->children, &proc->ptnode);

    release_spinlock(&global_process_lock);

}

int start_proc(Proc *p, void (*entry)(u64), u64 arg)
{
    if(debug_proc)printk("start_proc on CPU %lld\n",cpuid());
    acquire_spinlock(&global_process_lock);

    // 1. set the parent to root_proc if NULL
    if (p->parent == NULL) {
        p->parent = &root_proc;
        _detach_from_list(&p->ptnode);
        insert_into_list_lockfree(&root_proc.children, &p->ptnode);
    }

    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = arg;

    // 3. activate the proc and return its pid
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
        PANIC();
        // insert_list_into_list_lockfree(current->children.next,
        //                                current->children.prev,
        //                                &root_proc.children);
    }

    for (ListNode *p = current->zombie_children.next,
                  *p_next = current->zombie_children.next->next;
         p != &current->zombie_children; p = p_next) {
        Proc *proc = container_of(p, Proc, ptnode);
        proc->parent = &root_proc;
        p_next = p->next;
        _detach_from_list(p);
        insert_into_list_lockfree(&root_proc.zombie_children, p);
        post_sem(&root_proc.childexit);
    }

    // Remove self from parent's child list and add to zombie child list
    _detach_from_list(&current->ptnode);
    insert_into_list_lockfree(&current->parent->zombie_children,
                              &current->ptnode);

    // Notify parent
    post_sem(&current->parent->childexit);
    acquire_sched_lock();
    release_spinlock(&global_process_lock);

    sched(ZOMBIE);
    PANIC(); // prevent the warning of 'no_return function returns'
}

// DFS. Must call this function with process locked
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