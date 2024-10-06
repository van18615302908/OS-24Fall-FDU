#pragma once

#include <kernel/proc.h>
#include <common/rbtree.h>

#define NCPU 4

struct sched {
    // TODO: customize your sched info
    struct proc* running_proc;
    struct proc* idle;
};

struct sched_info
{
    ListNode rq;
};


struct cpu {
    bool online;
    struct rb_root_ timer;
    struct sched sched;
};

extern struct cpu cpus[NCPU];

void set_cpu_on();
void set_cpu_off();
