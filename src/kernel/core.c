#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <test/test.h>
#include <driver/virtio.h>

volatile bool panic_flag;

NO_RETURN void idle_entry()
{
    set_cpu_on();
    while (1) {
        yield();
        if (panic_flag)
            break;
        arch_with_trap
        {
            arch_wfi();
        }
    }
    set_cpu_off();
    arch_stop_cpu();
}

NO_RETURN void kernel_entry()
{
    init_filesystem();

    printk("Hello world! (Core %lld)\n", cpuid());
    // proc_test();
    // vm_test();
    // user_proc_test();
    // io_test();

    /* LAB 4 TODO 3 BEGIN */
    Buf mbr_buf;
    mbr_buf.block_no = 0;  // 读取 MBR
    mbr_buf.flags = 0;     // 读取操作
    virtio_blk_rw(&mbr_buf);  // 读取 MBR

    // 解析第二分区信息
    u8 *mbr_data = mbr_buf.data;
    u32 second_partition_lba = *(u32 *)(mbr_data + 0x1CE + 0x8);  // 第二分区起始 LBA
    u32 second_partition_size = *(u32 *)(mbr_data + 0x1CE + 0xC); // 第二分区大小

    printk("Second partition LBA: %u\n", second_partition_lba);//防止warning
    printk("Second partition size: %u blocks\n", second_partition_size);   
    /* LAB 4 TODO 3 END */

    /**
     * (Final) TODO BEGIN 
     * 
     * Map init.S to user space and trap_return to run icode.
     */


    /* (Final) TODO END */
}

NO_INLINE NO_RETURN void _panic(const char *file, int line)
{
    printk("=====%s:%d PANIC%lld!=====\n", file, line, cpuid());
    panic_flag = true;
    set_cpu_off();
    for (int i = 0; i < NCPU; i++) {
        if (cpus[i].online)
            i--;
    }
    printk("Kernel PANIC invoked at %s:%d. Stopped.\n", file, line);
    arch_stop_cpu();
}