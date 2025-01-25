#include <elf.h>
#include <common/string.h>
#include <common/defines.h>
#include <kernel/console.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/pt.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <aarch64/trap.h>
#include <fs/file.h>
#include <fs/inode.h>
#include <driver/memlayout.h>



extern int fdalloc(struct file *f);
extern void recycle_proc(Proc *proc);


//ELF 头验证
int validate_elf_header(Inode *inode, Elf64_Ehdr *elf_header) {
    if (inodes.read(inode, (u8 *)elf_header, 0, sizeof(Elf64_Ehdr)) != sizeof(Elf64_Ehdr)) {
        return -1;
    }
    u32 *elf_magic = (u32 *)(&elf_header->e_ident);
    if (*elf_magic != *((u32 *)ELFMAG)) {
        return -1;
    }
    return 0;
}

int load_elf_segments(Inode *inode, const Elf64_Ehdr *elf_header, struct pgdir *pgdir, u64 *heap_start) {
    Elf64_Phdr program_header;

    for (u16 ph_index = 0; ph_index < elf_header->e_phnum; ph_index++) {
        u64 offset = elf_header->e_phoff + ph_index * sizeof(Elf64_Phdr);
        struct section *section;

        // 读取程序头
        if (inodes.read(inode, (u8 *)&program_header, offset, sizeof(Elf64_Phdr)) != sizeof(Elf64_Phdr)) {
            printk("ELF program header read failure\n");
            return -1; // 出现错误返回
        }

        // 忽略不可加载的段
        if (program_header.p_type != PT_LOAD) {
            continue;
        }

        // 检查段大小是否有效
        if (program_header.p_memsz < program_header.p_filesz) {
            printk("memsz should not be smaller than filesz\n");
            return -1; // 无效段，返回错误
        }

        // 创建并初始化段
        section = (struct section *)kalloc(sizeof(struct section));
        section->begin = program_header.p_vaddr;
        section->end = program_header.p_vaddr + program_header.p_memsz;
        section->flags = 0;
        section->fp = NULL;

        // 根据标志设置段类型
        switch (program_header.p_flags) {
        case PF_R | PF_W:
            section->flags = ST_DATA; // 数据段或 BSS 段
            break;
        case PF_R | PF_X:
            ASSERT(program_header.p_memsz == program_header.p_filesz);
            section->flags = ST_TEXT; // 代码段
            break;
        default:
            printk("(warn) unrecognizable section type\n");
            kfree(section);
            return -1; // 无法识别的类型
        }

        // 将段插入段链表
        _insert_into_list(&pgdir->section_head, &section->stnode);

        // 更新堆的起始地址
        *heap_start = MAX(*heap_start, section->end);

        // 加载段内容到内存
        usize bytes_loaded = 0;
        u64 va_pos = section->begin;
        u64 file_offset = program_header.p_offset;
        while (bytes_loaded < program_header.p_filesz) {
            char *new_page = kalloc_page();
            
            u64 va_page_base = PAGE_BASE(va_pos);
            u64 va_offset_in_page = va_pos - va_page_base;
            u32 read_count = MIN(PAGE_SIZE - va_offset_in_page, program_header.p_filesz - bytes_loaded);
            read_count = inodes.read(inode, (u8 *)(new_page + va_offset_in_page), file_offset, read_count);
            vmmap(pgdir, va_page_base, new_page, PTE_USER_DATA);

            bytes_loaded += read_count;
            file_offset += read_count;
            va_pos += read_count;

            // 如果段内容之后是 BSS，填充剩余部分为 0
            if (bytes_loaded == program_header.p_filesz &&
                bytes_loaded < program_header.p_memsz && va_pos % PAGE_SIZE != 0) {
                va_offset_in_page = va_pos - PAGE_BASE(va_pos);
                u64 fill_count = PAGE_SIZE - va_offset_in_page;
                memset(new_page + va_offset_in_page, 0, fill_count);
                bytes_loaded += fill_count;
                va_pos += fill_count;
            }
        }

        // 如果段包含 BSS，初始化为 0
        if (program_header.p_filesz < program_header.p_memsz) {
            ASSERT(va_pos % PAGE_SIZE == 0);
            while (bytes_loaded < program_header.p_memsz) {
                vmmap(pgdir, va_pos, get_zero_page(), PTE_USER_DATA | PTE_RO);
                bytes_loaded += PAGE_SIZE;
                va_pos += PAGE_SIZE;
            }
        }
    }

    return 0; // 成功
}

struct section *initialize_heap(u64 heap_start, struct pgdir *pgdir) {
    heap_start = ALIGN_UP(heap_start, PAGE_SIZE);
    struct section *heap_section = (struct section *)kalloc(sizeof(struct section));
    heap_section->flags = ST_HEAP;
    heap_section->begin = heap_start;
    heap_section->end = heap_start; // 初始堆大小为 0
    heap_section->fp = NULL;
    _insert_into_list(&pgdir->section_head, &heap_section->stnode);
    return heap_section;
}


struct section *initialize_stack(struct pgdir *pgdir) {
    struct section *stack_section = (struct section *)kalloc(sizeof(struct section));
    stack_section->flags = ST_FILE;
    stack_section->end = PHYSTOP;
    stack_section->begin = stack_section->end - STACK_PAGE_COUNT * PAGE_SIZE; // 初始化 80k 堆栈
    stack_section->fp = NULL;
    _insert_into_list(&pgdir->section_head, &stack_section->stnode);
    return stack_section;
}


u64 setup_stack(struct pgdir *pgdir, struct section *stack_section, char *const argv[], char *const envp[]) {
    u64 argc = 0, envc = 0, strings_size = 0;

    // 计算参数和环境变量的总大小
    if (argv) {
        while (argv[argc]) {
            strings_size += strlen(argv[argc++]) + 1;
        }
    }
    if (envp) {
        while (envp[envc]) {
            strings_size += strlen(envp[envc++]) + 1;
        }
    }

    // 计算指针和对齐所需的空间
    const u64 pointer_size = (1 + (argc + 1) + (envc + 1)) * sizeof(u64);
    u64 padding_size = ALIGN_UP(strings_size, 8) - strings_size;
    if ((strings_size + padding_size + pointer_size) % 16 != 0) {
        padding_size += 8;
    }

    const u64 preallocated_stack_size = ALIGN_UP(
            pointer_size + padding_size + strings_size + STACK_BOTTOM_RESERVED,
            PAGE_SIZE);

    // 初始化空的栈部分
    for (u64 q = stack_section->begin;
         q < stack_section->end - preallocated_stack_size; q += PAGE_SIZE) {
        vmmap(pgdir, q, get_zero_page(), PTE_USER_DATA | PTE_RO);
    }

    u64 strings_pos = stack_section->end - STACK_BOTTOM_RESERVED;
    u64 sp = strings_pos - strings_size - padding_size;
    const u64 zero = 0;

    // 复制环境变量
    sp -= sizeof(char *);
    copyout(pgdir, (void *)sp, (void *)(&zero), sizeof(char *));
    for (i64 i = envc - 1; i >= 0; i--) {
        u32 len = strlen(envp[i]) + 1;
        sp -= sizeof(char *);
        strings_pos -= len;
        copyout(pgdir, (void *)strings_pos, (void *)envp[i], len);
        copyout(pgdir, (void *)sp, (void *)(&strings_pos), sizeof(char *));
    }

    // 复制参数
    sp -= sizeof(char *);
    copyout(pgdir, (void *)sp, (void *)(&zero), sizeof(char *));
    for (i64 i = argc - 1; i >= 0; i--) {
        u32 len = strlen(argv[i]) + 1;
        sp -= sizeof(char *);
        strings_pos -= len;
        copyout(pgdir, (void *)strings_pos, (void *)argv[i], len);
        copyout(pgdir, (void *)sp, (void *)(&strings_pos), sizeof(char *));
    }

    // 复制 argc
    sp -= sizeof(argc);
    copyout(pgdir, (void *)sp, (void *)(&argc), sizeof(argc));

    ASSERT(sp % 16 == 0);
    return sp;
}


void switch_to_new_process(struct pgdir *new_pgdir, u64 sp, u64 entry) {
    Proc *this = thisproc();
    this->ucontext->sp = sp;
    this->ucontext->elr = entry;

    // 释放当前进程的旧页表
    free_sections(&this->pgdir);
    free_pgdir(&this->pgdir);

    // 切换到新的页表
    memcpy(&this->pgdir, new_pgdir, sizeof(struct pgdir));
    _insert_into_list(&new_pgdir->section_head, &this->pgdir.section_head);
    _detach_from_list(&new_pgdir->section_head);
    attach_pgdir(&this->pgdir);
}


int execve(const char *path, char *const argv[], char *const envp[])
{
    /* (Final) TODO BEGIN */

    OpContext ctx;
    bcache.begin_op(&ctx);
    Inode *inode = namei(path, &ctx);

    if (!inode) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.lock(inode);
    Elf64_Ehdr elf_header;
    //ELF 头验证
    int ret = validate_elf_header(inode, &elf_header);
    if (ret != 0) {
        inodes.unlock(inode);
        inodes.put(&ctx, inode);
        bcache.end_op(&ctx);
        return -1;
    }


    //初始化页表与段信息
    struct pgdir new_pgdir;
    init_pgdir(&new_pgdir);
    init_sections(&new_pgdir.section_head);

    // Elf64_Phdr program_header;


    //解析程序头和加载段
    u64 heap_start = 0;
    if (load_elf_segments(inode, &elf_header, &new_pgdir, &heap_start) != 0) {
        free_sections(&new_pgdir);
        free_pgdir(&new_pgdir);
        inodes.unlock(inode);
        inodes.put(&ctx, inode);
        bcache.end_op(&ctx);
        return -1;
    }


    inodes.unlock(inode);
    inodes.put(&ctx, inode);
    bcache.end_op(&ctx);

    heap_start = ALIGN_UP(heap_start, PAGE_SIZE);

    //初始化堆
    initialize_heap(heap_start, &new_pgdir);

    struct section *stack_section = initialize_stack(&new_pgdir);

    u64 sp = setup_stack(&new_pgdir, stack_section, argv, envp);

    switch_to_new_process(&new_pgdir, sp, elf_header.e_entry);

    // printk("Execve finished\n");
    return 0;
    /* (Final) TODO END */
}
