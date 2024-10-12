#include <kernel/syscall.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <common/sem.h>
#include <test/test.h>
#include <aarch64/intrinsic.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"

void *syscall_table[NR_SYSCALL] = {
    [0 ... NR_SYSCALL - 1] = NULL,
    [SYS_myreport] = (void *)syscall_myreport,
};

void syscall_entry(UserContext *context)
{
    // TODO
    // Invoke syscall_table[id] with args and set the return value.
    // id is stored in x8. args are stored in x0-x5. return value is stored in x0.
    // be sure to check the range of id. if id >= NR_SYSCALL, panic.
    u64 id = 0, ret = 0;
    id = context->x[8];
    if(id >= NR_SYSCALL){
        PANIC();
    }else{
        u64 (*p) (u64, u64, u64, u64, u64, u64) = syscall_table[id];
        ret = p(context->x[0], context->x[1], context->x[2], context->x[3], context->x[4], context->x[5]);
        context->x[0] = ret;
    }
}

#pragma GCC diagnostic pop