#include <common/spinlock.h>

#define PID_MAX ((unsigned)__INT16_MAX__)
#define PID_BYTE ((PID_MAX + 7) >> 3)

void init_pid_pool();

int allocate_pid();

void free_pid(int pid);