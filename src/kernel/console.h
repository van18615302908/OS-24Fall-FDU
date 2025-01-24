#pragma once
#include <common/defines.h>
#include <fs/inode.h>
#define CMD_HISTORY_SIZE 16
#define CMD_MAX_LENGTH 128

#define IBUF_SIZE 128
#define C(x) ((x) - '@') // Control-x

struct console {
    SpinLock lock;
    Semaphore sem;
    char buf[IBUF_SIZE];
    usize read_idx;
    usize write_idx;
    usize edit_idx;

    char cmd_history[CMD_HISTORY_SIZE][CMD_MAX_LENGTH]; // 保存历史命令
    int cmd_history_count; // 当前历史命令总数
    int cmd_history_index; // 当前正在浏览的历史命令索引
    int cmd_history_viewing; // 标志是否正在浏览历史记录

};

void console_init();
void console_intr(char c);
isize console_write(Inode *ip, char *buf, isize n);
isize console_read(Inode *ip, char *dst, isize n);