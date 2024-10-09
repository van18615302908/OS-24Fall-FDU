#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>

Proc root_proc;

void kernel_entry();
void proc_entry();

// 定义全局锁
static SpinLock global_lock;

int debug_fyy = 0;

//pidmap
typedef struct pidmap
{
    unsigned int nr_free;
    char page[4096];
} pidmap_t;

#define PID_MAX_DEFAULT 0x8000
#define BITS_PER_BYTE 8
#define BITS_PER_PAGE (PAGE_SIZE * BITS_PER_BYTE)
#define BITS_PER_PAGE_MASK (BITS_PER_PAGE - 1)
static pidmap_t pidmap = { PID_MAX_DEFAULT, {0}};
static int last_pid = -1;

static int test_and_set_bit(int offset, void *addr)
{
    unsigned long mask = 1UL << (offset & (sizeof(unsigned long) * BITS_PER_BYTE - 1));
    unsigned long *p = ((unsigned long*)addr) + (offset >> (sizeof(unsigned long) + 1));
    unsigned long old = *p;
 
    *p = old | mask;
 
    return (old & mask) != 0;
}

static void clear_bit(int offset, void *addr)
{
    unsigned long mask = 1UL << (offset & (sizeof(unsigned long) * BITS_PER_BYTE - 1));
    unsigned long *p = ((unsigned long*)addr) + (offset >> (sizeof(unsigned long) + 1));
    unsigned long old = *p;
    *p = old & ~mask;
}

static int find_next_zero_bit(void *addr, int size, int offset)
{
    unsigned long *p;
    unsigned long mask;
 
    while (offset < size)
    {
        p = ((unsigned long*)addr) + (offset >> (sizeof(unsigned long) + 1));
        mask = 1UL << (offset & (sizeof(unsigned long) * BITS_PER_BYTE - 1));
 
        if ((~(*p) & mask))
        {
            break;
        }
        ++offset;
    }
 
    return offset;
}

static int alloc_pidmap()
{
    int pid = last_pid + 1;
    int offset = pid & BITS_PER_PAGE_MASK;
    
    if (!pidmap.nr_free)
    {
        return -1;
    }
 
    offset = find_next_zero_bit(&pidmap.page, BITS_PER_PAGE, offset);
    if(offset == BITS_PER_PAGE) offset = find_next_zero_bit(&pidmap.page, offset-1, 30);
    if (BITS_PER_PAGE != offset && !test_and_set_bit(offset, &pidmap.page))
    {
        --pidmap.nr_free;
        last_pid = offset;
        return offset;
    }
 
    return -1;
}

static void free_pidmap(int pid)
{
    int offset = pid & BITS_PER_PAGE_MASK;
 
    if(pid > 29)pidmap.nr_free++;
    clear_bit(offset,  &pidmap.page);
}





// init_kproc initializes the kernel process
// NOTE: should call after kinit
void init_kproc()
{
    // TODO:
    // 1. init global resources (e.g. locks, semaphores)
    // 2. init the root_proc (finished)
    if(debug_fyy)printk("init_kproc\n");

    // 初始化全局锁
    init_spinlock(&global_lock);


    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);
}

void init_proc(Proc *p)
{
    // TODO:
    // setup the Proc with kstack and pid allocated
    // NOTE: be careful of concurrency
    if(debug_fyy)printk("init_proc\n");

    p->killed = false;
    p->idle = false;
    acquire_spinlock(&global_lock);
    p->pid = alloc_pidmap();
    release_spinlock(&global_lock);
    p->state = UNUSED;
    init_sem(&(p->childexit),0);
    init_list_node(&(p->children));
    init_list_node(&(p->ptnode));
    p->parent = NULL;
    init_schinfo(&(p->schinfo));
    p->kstack = kalloc_page();
    p->ucontext = p->kstack + PAGE_SIZE - 16 - sizeof(UserContext);
    p->kcontext = p->kstack + PAGE_SIZE - 16 - sizeof(UserContext) - sizeof(KernelContext);
}

Proc *create_proc()
{
    if(debug_fyy)printk("create_proc\n");
    Proc *p = kalloc(sizeof(Proc));
    init_proc(p);
    return p;
}

void set_parent_to_this(Proc *proc)
{
    // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL
    if(debug_fyy)printk("set_parent_to_this\n");
    ASSERT(proc->parent == NULL);
    acquire_spinlock(&global_lock);
    proc->parent = thisproc();
    _insert_into_list(&thisproc()->children, &proc->ptnode);
    release_spinlock(&global_lock);
}

int start_proc(Proc *p, void (*entry)(u64), u64 arg)
{
    // TODO:
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its pid
    // NOTE: be careful of concurrency
    if(debug_fyy)printk("start_proc\n");
    if(p->parent == NULL){
        acquire_spinlock(&global_lock);
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
        release_spinlock(&global_lock);
    }
    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = (u64)arg;
    int id = p->pid;
    activate_proc(p);
    return id;
}

int wait(int *exitcode)
{
    // TODO:
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its pid and exitcode
    // NOTE: be careful of concurrency
    if(debug_fyy)printk("wait\n");

    auto this = thisproc();
    if(_empty_list(&this->children))
        return -1;
    wait_sem(&this->childexit);
    acquire_spinlock(&global_lock);
    auto p = this->children.prev;
    auto proc = container_of(p, struct Proc, ptnode);
    if(is_zombie(proc)){
        *exitcode = proc->exitcode;
        int id = proc->pid;
        _detach_from_list(p);
        kfree_page(proc->kstack);
        kfree(proc);
        free_pidmap(id);
        release_spinlock(&global_lock);
        return id;
    }
    PANIC();
    release_spinlock(&global_lock);
    return -1;
}

NO_RETURN void exit(int code)
{
    // TODO:
    // 1. set the exitcode
    // 2. clean up the resources
    // 3. transfer children to the root_proc, and notify the root_proc if there is zombie
    // 4. sched(ZOMBIE)
    // NOTE: be careful of concurrency
    if(debug_fyy)printk("exit\n");
    auto this = thisproc();
    this->exitcode = code;
    //TODO clean up file resources
    acquire_spinlock(&global_lock);
    ListNode* pre = NULL;
    _for_in_list(p, &this->children){
        if(pre != NULL && pre != &this->children){
            auto proc = container_of(pre, struct Proc, ptnode);
            proc->parent = &root_proc;
            auto t = &root_proc.children;
            if(is_zombie(proc)){
                pre->prev = t->prev;
                pre->next = t;
                t->prev->next = pre;
                t->prev = pre;
                post_sem(&root_proc.childexit);
            }else{
                _insert_into_list(t, pre);
            }
        }
        pre = p;
    }
    init_list_node(&this->children);
    pre = &this->ptnode;
    _detach_from_list(pre);
    auto t = &this->parent->children;
    pre->prev = t->prev;
    pre->next = t;
    t->prev->next = pre;
    t->prev = pre;
    post_sem(&this->parent->childexit);
    release_spinlock(&global_lock);
    sched( ZOMBIE);
    PANIC(); // prevent the warning of 'no_return function returns'
}
