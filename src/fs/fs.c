#include <fs/block_device.h>
#include <fs/cache.h>
#include <fs/defines.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <fs/file.h>
#include <common/defines.h>
#include <kernel/printk.h>

void init_filesystem() {
    init_block_device();
    printk("init_block_device done\n");

    const SuperBlock* sblock = get_super_block();
    printk("get_super_block done\n");
    init_bcache(sblock, &block_device);
    printk("init_bcache done\n");
    init_inodes(sblock, &bcache);
    printk("init_inodes done\n");
    init_ftable();
    printk("init_ftable done\n");
}
