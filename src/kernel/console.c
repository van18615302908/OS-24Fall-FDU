#include <kernel/console.h>
#include <aarch64/intrinsic.h>
#include <kernel/sched.h>
#include <driver/uart.h>
#include <common/string.h>
#include <kernel/printk.h>

#define CTRL(c) (c - '@')

struct console cons;

void console_init()
{
    /* (Final) TODO BEGIN */
    init_spinlock(&cons.lock);
    init_sem(&cons.sem, 0);

    cons.read_idx = 0;
    cons.write_idx = 0;
    cons.edit_idx = 0;
    /* (Final) TODO END */
}

/**
 * console_write - write to uart from the console buffer.
 * @ip: the pointer to the inode
 * @buf: the buffer
 * @n: number of bytes to write
 */
isize console_write(Inode *ip, char *buf, isize n)
{
    /* (Final) TODO BEGIN */
    inodes.unlock(ip);
    acquire_spinlock(&cons.lock);
    for (isize i = 0; i < n; i++) {
        uart_put_char(buf[i]);
    }
    release_spinlock(&cons.lock);
    inodes.lock(ip);

    return n;
    /* (Final) TODO END */
}

/**
 * console_read - read to the destination from the buffer
 * @ip: the pointer to the inode
 * @dst: the destination
 * @n: number of bytes to read
 */
isize console_read(Inode *ip, char *dst, isize n)
{
    /* (Final) TODO BEGIN */

    isize len = n;
    inodes.unlock(ip);
    acquire_spinlock(&cons.lock);

    while (len > 0) {
        // Nothing new to read
        while (cons.read_idx == cons.write_idx) {
            _lock_sem(&cons.sem);
            release_spinlock(&cons.lock);
            if (!_wait_sem(&cons.sem, 1)) {
                // Process already killed
                inodes.lock(ip);
                return -1;
            }
            acquire_spinlock(&cons.lock);
        }

        char c = cons.buf[cons.read_idx % IBUF_SIZE];
        cons.read_idx++;

        // EOF
        if (c == CTRL('D')) {
            if (len < n) {
                // From xv6:
                // Save ^D for next time, to make sure caller gets a 0-byte result.
                cons.read_idx--;
            }
            break;
        }

        *dst = c;
        dst++;
        len--;

        // Endl
        if (c == '\n') {
            break;
        }
    }

    release_spinlock(&cons.lock);
    inodes.lock(ip);
    return n - len;
    /* (Final) TODO END */
}

void console_intr(char c)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&cons.lock);
    // Special characters
    switch (c) {
    // case '\n': // 用户按下 Enter 键
    //     // 存储到历史记录
    //     if (cons.edit_idx > cons.write_idx) {
    //         int cmd_len = cons.edit_idx - cons.write_idx;
    //         if (cmd_len > CMD_MAX_LENGTH - 1) {
    //             cmd_len = CMD_MAX_LENGTH - 1;
    //         }
    //         strncpy(cons.cmd_history[cons.cmd_history_count % CMD_HISTORY_SIZE],
    //                 &cons.buf[cons.write_idx % IBUF_SIZE], cmd_len);
    //         cons.cmd_history[cons.cmd_history_count % CMD_HISTORY_SIZE][cmd_len] = '\0';
    //         cons.cmd_history_count++;
    //     }

    //     cons.write_idx = cons.edit_idx;
    //     uart_put_char(c);
    //     post_all_sem(&cons.sem); // 处理缓冲
    //     break;

    // case CTRL('h'): // control + i
    //     if (cons.cmd_history_viewing == 0) {
    //         cons.cmd_history_index = cons.cmd_history_count;
    //         cons.cmd_history_viewing = 1;
    //     }
    //     if (cons.cmd_history_index > 0) {
    //         cons.cmd_history_index--;
    //         // 清除当前行
    //         while (cons.edit_idx > cons.write_idx) {
    //             cons.edit_idx--;
    //             uart_put_char('\b');
    //             uart_put_char(' ');
    //             uart_put_char('\b');
    //         }
    //         // 加载历史命令
    //         const char *cmd = cons.cmd_history[cons.cmd_history_index % CMD_HISTORY_SIZE];
    //         int cmd_len = strlen(cmd);
    //         for (int i = 0; i < cmd_len; i++) {
    //             cons.buf[cons.edit_idx++ % IBUF_SIZE] = cmd[i];
    //             uart_put_char(cmd[i]);
    //         }
    //     }
    //     break;


    // case CTRL('M'): // control + 
    //     if (cons.cmd_history_viewing == 1 && cons.cmd_history_index < cons.cmd_history_count) {
    //         cons.cmd_history_index++;
    //         // 清除当前行
    //         while (cons.edit_idx > cons.write_idx) {
    //             cons.edit_idx--;
    //             uart_put_char('\b');
    //             uart_put_char(' ');
    //             uart_put_char('\b');
    //         }
    //         // 加载历史命令或清空行
    //         if (cons.cmd_history_index == cons.cmd_history_count) {
    //             cons.cmd_history_viewing = 0; // 已到最新，无命令可显示
    //         } else {
    //             const char *cmd = cons.cmd_history[cons.cmd_history_index % CMD_HISTORY_SIZE];
    //             int cmd_len = strlen(cmd);
    //             for (int i = 0; i < cmd_len; i++) {
    //                 cons.buf[cons.edit_idx++ % IBUF_SIZE] = cmd[i];
    //                 uart_put_char(cmd[i]);
    //             }
    //         }
    //     }
    //     break;


    case '\x7f'://backspace
        if (cons.edit_idx != cons.write_idx) {
            cons.edit_idx--;
            uart_put_char('\b');
            uart_put_char(' ');
            uart_put_char('\b');
        }
        break;
    case CTRL('U')://ctrl+u
        while (cons.edit_idx != cons.write_idx &&
               cons.buf[(cons.edit_idx + IBUF_SIZE - 1) % IBUF_SIZE] != '\n') {
            cons.edit_idx--;
            // Clear console
            uart_put_char('\b');
            uart_put_char(' ');
            uart_put_char('\b');
        }
        break;
    case CTRL('D')://ctrl+d
        cons.write_idx = cons.edit_idx;
        __attribute__((fallthrough));
    default:
        // Handle enter key as new line
        if (c == '\r') {
            c = '\n';
        }

        cons.buf[cons.edit_idx++ % IBUF_SIZE] = c;
        uart_put_char(c);

        // Flush
        if (c == '\n' || c == CTRL('D')) {
            cons.write_idx = cons.edit_idx;
            post_all_sem(&cons.sem);
        }
        break;
    }
    release_spinlock(&cons.lock);
    /* (Final) TODO END */
}
