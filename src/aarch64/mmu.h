#pragma once

#include <common/defines.h>
typedef unsigned long long u64; // 定义 64 位无符号整型为 u64
#define PAGE_SIZE 4096 // 定义页面大小为 4096 字节（4KB）

/* 内存区域属性 */
#define MT_DEVICE_nGnRnE 0x0 // 设备内存，nGnRnE（non-Gathering, non-Reordering, no Early Write Acknowledgement）
#define MT_NORMAL 0x1 // 普通内存
#define MT_NORMAL_NC 0x2 // 非缓存的普通内存
#define MT_DEVICE_nGnRnE_FLAGS 0x00 // 设备内存的标志
#define MT_NORMAL_FLAGS \
    0xFF // 内存标志：内部/外部写回，非瞬态，可读写分配
#define MT_NORMAL_NC_FLAGS 0x44 // 内存标志：内部/外部非缓存

/* 共享属性 */
#define SH_OUTER (2 << 8) // 外部共享标志，位于 PTE 的第 10 到第 11 位
#define SH_INNER (3 << 8) // 内部共享标志

/* 访问标志 */
#define AF_USED (1 << 10) // 访问标志（Access Flag），标记页表项已被访问

/* 页表项的类型和属性 */
#define PTE_NORMAL_NC ((MT_NORMAL_NC << 2) | AF_USED | SH_OUTER) // 非缓存普通内存页表项
#define PTE_NORMAL ((MT_NORMAL << 2) | AF_USED | SH_OUTER) // 普通内存页表项
#define PTE_DEVICE ((MT_DEVICE_nGnRnE << 2) | AF_USED) // 设备内存页表项

/* 页表项有效性 */
#define PTE_VALID 0x1 // 页表项有效

/* 页表项类型 */
#define PTE_TABLE 0x3 // 页表项指向下一级页表
#define PTE_BLOCK 0x1 // 页表项表示内存块
#define PTE_PAGE 0x3 // 页表项表示页面

/* 权限标志 */
#define PTE_KERNEL (0 << 6) // 内核态访问
#define PTE_USER (1 << 6) // 用户态访问
#define PTE_RO (1 << 7) // 只读权限
#define PTE_RW (0 << 7) // 可读写权限

/* 内核数据页表项属性 */
#define PTE_KERNEL_DATA (PTE_KERNEL | PTE_NORMAL | PTE_BLOCK) // 内核数据内存块
#define PTE_KERNEL_DEVICE (PTE_KERNEL | PTE_DEVICE | PTE_BLOCK) // 内核设备内存块
#define PTE_USER_DATA (PTE_USER | PTE_NORMAL | PTE_PAGE) // 用户数据页面

/* 一个页表中的页表项数量 */
#define N_PTE_PER_TABLE 512 // 每个页表中有 512 个页表项

/* 页表项高位的 NX （No Execute，不可执行）标志 */
#define PTE_HIGH_NX (1LL << 54) // 不可执行标志，位于页表项的第 54 位

/* 内核地址空间掩码 */
#define KSPACE_MASK 0xFFFF000000000000 // 用于掩码高地址空间的 48 位地址（用于区分内核和用户地址空间）

/* 将内核虚拟地址转换为物理地址 */
#define K2P(addr) ((u64)(addr) - (KSPACE_MASK)) // 内核虚拟地址转物理地址
    0xFF /* Inner/Outer Write-Back Non-Transient RW-Allocate */
#define MT_NORMAL_NC_FLAGS 0x44 /* Inner/Outer Non-Cacheable */

#define SH_OUTER (2 << 8)
#define SH_INNER (3 << 8)

#define AF_USED (1 << 10)

#define PTE_NORMAL_NC ((MT_NORMAL_NC << 2) | AF_USED | SH_OUTER)
#define PTE_NORMAL ((MT_NORMAL << 2) | AF_USED | SH_OUTER)
#define PTE_DEVICE ((MT_DEVICE_nGnRnE << 2) | AF_USED)

#define PTE_VALID 0x1

#define PTE_TABLE 0x3
#define PTE_BLOCK 0x1
#define PTE_PAGE 0x3

#define PTE_KERNEL (0 << 6)
#define PTE_USER (1 << 6)
#define PTE_RO (1 << 7)
#define PTE_RW (0 << 7)

#define PTE_KERNEL_DATA (PTE_KERNEL | PTE_NORMAL | PTE_BLOCK)
#define PTE_KERNEL_DEVICE (PTE_KERNEL | PTE_DEVICE | PTE_BLOCK)
#define PTE_USER_DATA (PTE_USER | PTE_NORMAL | PTE_PAGE)

#define N_PTE_PER_TABLE 512

#define PTE_HIGH_NX (1LL << 54)

#define KSPACE_MASK 0xFFFF000000000000

// convert kernel address into physical address.
#define K2P(addr) ((u64)(addr) - (KSPACE_MASK))

/* 将物理地址转换为内核虚拟地址 */
#define P2K(addr) ((u64)(addr) + (KSPACE_MASK)) // 物理地址转内核虚拟地址

/* 将任意地址转换为内核地址空间 */
#define KSPACE(addr) ((u64)(addr) | (KSPACE_MASK)) // 任意地址转内核虚拟地址

/* 将任意地址转换为物理地址空间 */
#define PSPACE(addr) ((u64)(addr) & (~KSPACE_MASK)) // 任意地址转物理地址

/* 页表项类型定义 */
typedef u64 PTEntry; // 定义页表项为 64 位无符号整型
typedef PTEntry PTEntries[N_PTE_PER_TABLE]; // 定义一个页表数组，每个页表有 512 个页表项
typedef PTEntry *PTEntriesPtr; // 页表项指针类型

/* 地址相关的操作宏 */
#define VA_OFFSET(va) ((u64)(va) & 0xFFF) // 获取虚拟地址中的页内偏移（低 12 位）
#define PTE_ADDRESS(pte) ((pte) & ~0xFFFF000000000FFF) // 获取页表项中的物理地址（去掉高地址位和标志位）
#define PTE_FLAGS(pte) ((pte) & 0xFFFF000000000FFF) // 获取页表项中的标志位
#define P2N(addr) (addr >> 12) // 将物理地址转换为页号（地址右移 12 位）
#define PAGE_BASE(addr) ((u64)addr & ~(PAGE_SIZE - 1)) // 将地址对齐到页面基址（去掉低 12 位）