#include <aarch64/intrinsic.h>
#include <driver/uart.h>
#include <kernel/printk.h>
#include <kernel/core.h>
#include <common/string.h>

static volatile bool boot_secondary_cpus = false;


void main()
{
    if (cpuid() == 0) {
        /* @todo: Clear BSS section.*/

        extern char edata[], end[];
        memset(edata, 0, (usize)(end - edata));

        smp_init();
        uart_init();
        printk_init();

        /* @todo: Print "Hello, world! (Core 0)" */
        // uart_put_char('Hello, world! (Core 0)');

        const char *message = "Hello, world! (Core 0)\n";
        for (int i = 0; message[i]!= '\0'; i++) {
            uart_put_char(message[i]);
        }
        arch_fence();

        // Set a flag indicating that the secondary CPUs can start executing.
        boot_secondary_cpus = true;
    } else {
        while (!boot_secondary_cpus)
            ;
        arch_fence();

        /* @todo: Print "Hello, world! (Core <core id>)" */
        const char *message = "Hello, world! (Core ";
        for (int i = 0; message[i]!= '\0'; i++) {
            uart_put_char(message[i]);
        }
        uart_put_char((char)('0' + cpuid())); // 将核心编号转换为对应的字符并输出
        uart_put_char(')');
        uart_put_char('\n');
    }

    set_return_addr(idle_entry);
}
