#include <test/test.h>
#include <common/rc.h>
#include <kernel/pt.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <common/sem.h>
#include <kernel/proc.h>
#include <kernel/syscall.h>
#include <driver/memlayout.h>
#include <kernel/sched.h>

PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc);

void vm_test()
{
    printk("vm_test\n");
    static void *p[100000];
    extern RefCount kalloc_page_cnt;
    struct pgdir pg;
    int p0 = kalloc_page_cnt.count;
    init_pgdir(&pg);
    for (u64 i = 0; i < 100000; i++) {
        p[i] = kalloc_page();
        *get_pte(&pg, i << 12, true) = K2P(p[i]) | PTE_USER_DATA;
        *(int *)p[i] = i;
    }
    attach_pgdir(&pg);
    for (u64 i = 0; i < 100000; i++) {
        ASSERT(*(int *)(P2K(PTE_ADDRESS(*get_pte(&pg, i << 12, false)))) ==
               (int)i);
        ASSERT(*(int *)(i << 12) == (int)i);
    }
    free_pgdir(&pg);
    attach_pgdir(&pg);
    for (u64 i = 0; i < 100000; i++)
        kfree_page(p[i]);
    ASSERT(kalloc_page_cnt.count == p0);
    printk("vm_test PASS\n");
}

void trap_return(u64);

static u64 proc_cnt[22] = { 0 }, cpu_cnt[4] = { 0 };
static Semaphore myrepot_done;

u64 syscall_myreport(u64 id)
{
    static bool stop;
    ASSERT(id < 22);
    if (stop)
        return 0;
    proc_cnt[id]++;
    cpu_cnt[cpuid()]++;
    if (proc_cnt[id] > 12345) {
        stop = true;
        post_sem(&myrepot_done);
    }
    return 0;
}

void user_proc_test()
{
    printk("user_proc_test\n");
    init_sem(&myrepot_done, 0);
    extern char loop_start[], loop_end[];
    int pids[22];
    for (int i = 0; i < 22; i++) {
        auto p = create_proc();
        for (u64 q = (u64)loop_start; q < (u64)loop_end; q += PAGE_SIZE) {
            *get_pte(&p->pgdir, EXTMEM + q - (u64)loop_start, true) =
                    K2P(q) | PTE_USER_DATA;
        }
        ASSERT(p->pgdir.pt);

        // TODO: setup the user context
        // 1. set x0 = i
        // 2. set elr = EXTMEM
        // 3. set spsr = 0
        p->ucontext->x[0] = i;
        p->ucontext->elr = EXTMEM;
        p->ucontext->spsr = 0;

        pids[i] = start_proc(p, trap_return, 0);
        printk("pid[%d] = %d\n", i, pids[i]);
    }
    ASSERT(wait_sem(&myrepot_done));
    printk("done\n");
    for (int i = 0; i < 22; i++)
        ASSERT(kill(pids[i]) == 0);
    for (int i = 0; i < 22; i++) {
        int code;
        int pid = wait(&code);
        printk("pid %d killed\n", pid);
        ASSERT(code == -1);
    }
    printk("user_proc_test PASS\nRuntime:\n");
    for (int i = 0; i < 4; i++)
        printk("CPU %d: %llu\n", i, cpu_cnt[i]);
    for (int i = 0; i < 22; i++)
        printk("Proc %d: %llu\n", i, proc_cnt[i]);
}
