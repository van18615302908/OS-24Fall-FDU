#include <kernel/console.h>
#include <aarch64/intrinsic.h>
#include <kernel/sched.h>
#include <driver/uart.h>
#include<driver/interrupt.h>

struct console cons;

void console_interrupt_handler(){
    console_intr(uart_get_char());
}//may_bug 

void console_init()
{
    /* (Final) TODO BEGIN */
    init_spinlock(&cons.lock);
    init_sem(&cons.sem, 0);
    // set_interrupt_handler(UART_IRQ,console_interrupt_handler);
    
    //bug
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
    ASSERT(ip->entry.type == INODE_DEVICE);
    inodes.unlock(ip);
    acquire_spinlock(&cons.lock);
    for(int i = 0; i < n; i++){
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
    ASSERT(ip->entry.type == INODE_DEVICE);
    inodes.unlock(ip);
    isize i = 0;
    acquire_spinlock(&cons.lock);
    while(i != n){
        while(cons.read_idx == cons.write_idx){
            _lock_sem(&cons.sem);
            release_spinlock(&cons.lock);
            if(_wait_sem(&cons.sem, true) == false){
                inodes.lock(ip);
                return -1;
            }
            acquire_spinlock(&cons.lock);
        }
        if(cons.buf[cons.read_idx % IBUF_SIZE] == C('D')) break;
        dst[i++] = cons.buf[cons.read_idx++ % IBUF_SIZE];
        if(dst[i-1] == '\n') break;
    }
    if(i == 0 && cons.buf[cons.read_idx % IBUF_SIZE] == C('D')) cons.read_idx++;
    release_spinlock(&cons.lock);
    inodes.lock(ip);
    return i;
    /* (Final) TODO END */
}

void console_intr(char c)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&cons.lock);
    while(c  != 0xff){
        switch(c){
            case '\x7f':
                if(cons.edit_idx != cons.write_idx){
                    cons.edit_idx--;
                    uart_put_char('\b');// 回显退格
                    uart_put_char(' ');// 覆盖字符
                    uart_put_char('\b');
                    break;
                }

            case C('U'):
                while(cons.edit_idx != cons.write_idx){// 删除整行内容
                    cons.edit_idx--;
                    uart_put_char('\b');
                    uart_put_char(' ');
                    uart_put_char('\b');
                }
                break;
            case C('D'): // 处理 Ctrl+D
                cons.buf[cons.edit_idx % IBUF_SIZE] = c; // 将 Ctrl+D 写入缓冲区
                cons.edit_idx++;
                uart_put_char(c); 
                cons.write_idx = cons.edit_idx; // 更新写入索引，表示 EOF
                post_all_sem(&cons.sem); // 唤醒等待的进程
                break;

            case C('C'):
                if(!thisproc()->idle){
                    ASSERT(kill(thisproc()->pid) != -1);
                }
                break;

            default:
                if(cons.read_idx + IBUF_SIZE != cons.edit_idx){
                    if(c == '\r') c = '\n';
                    cons.buf[cons.edit_idx++ % IBUF_SIZE] = c;
                    uart_put_char(c);
                    if(c == '\n' || c == C('D')){
                        cons.write_idx = cons.edit_idx;
                        post_all_sem(&cons.sem);
                    }
                }
                break;
        }
    }
    release_spinlock(&cons.lock);
    /* (Final) TODO END */
}