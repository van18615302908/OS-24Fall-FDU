#include <kernel/pid.h>
#include <common/string.h>
#include <common/bitmap.h>

typedef struct PidPool {
    SpinLock lock;
    Bitmap(bitmap, PID_MAX);
    u64 current_byte;
} PidPool;

static PidPool pid_pool;

void init_pid_pool()
{
    ASSERT(__UINT64_MAX__ > PID_BYTE);
    memset(&pid_pool, 0, sizeof(pid_pool));
    init_spinlock(&pid_pool.lock);
}

int allocate_pid()
{
    acquire_spinlock(&pid_pool.lock);

    u64 old_byte = pid_pool.current_byte;
    while (((u8 *)pid_pool.bitmap)[pid_pool.current_byte] == 0xFF) {
        pid_pool.current_byte = (pid_pool.current_byte + 1) % PID_BYTE;
        ASSERT(pid_pool.current_byte != old_byte);
    }

    for (int i = 0; i < 8; i++) {
        int pid = pid_pool.current_byte * 8 + i;
        if (!bitmap_get((BitmapCell *)&pid_pool.bitmap, pid)) {
            bitmap_set((BitmapCell *)&pid_pool.bitmap, pid);
            release_spinlock(&pid_pool.lock);
            return pid;
        }
    }
    PANIC();
}

void free_pid(int pid)
{
    acquire_spinlock(&pid_pool.lock);
    ASSERT(bitmap_get((BitmapCell *)&pid_pool.bitmap, pid));
    bitmap_clear((BitmapCell *)&pid_pool.bitmap, pid);
    release_spinlock(&pid_pool.lock);
}