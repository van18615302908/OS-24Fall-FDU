// #include "pid.h"

// void hashmap_init(hash_map map){
//     for(int i=0; i<HASHSIZE; i++)
//         map->bullet[i] = NULL;
// }

// int hashmap_insert(hash_node node, hash_map map, int (*hash)(hash_node node)){
//     node->next = NULL;
//     int b = hash(node);
//     hash_node t = map->bullet[b];
//     if(t == NULL){
//         map->bullet[b] = node;
//         return 0;
//     }
//     while(t->next != NULL){
//         if(t == node) return -1;
//         t = t->next;
//     }
//     t->next = node;
//     return 0;
// }

// void hashmap_erase(hash_node node, hash_map map, int (*hash)(hash_node node)){
//     int b = hash(node);
//     hash_node pre = NULL;
//     hash_node cur = map->bullet[b];
//     while(cur){
//         if(cur == node){
//             if(pre == NULL) map->bullet[b] = cur->next;
//             else pre->next = cur->next;
//             break;
//         }
//         pre = cur;
//         cur = cur->next;
//     }
// }

// hash_node hashmap_lookup(hash_node node, hash_map map, int (*hash)(hash_node node), bool (*hashcmp)(hash_node node1, hash_node node2)){
//     int b = hash(node);
//     hash_node cur = map->bullet[b];
//     while(cur){
//         if(hashcmp(cur, node)) return cur;
//         cur = cur->next;
//     }
//     return NULL;
// }

// int test_and_set_bit(int offset, void *addr)
// {
//     unsigned long mask = 1UL << (offset & (sizeof(unsigned long) * BITS_PER_BYTE - 1));
//     unsigned long *p = ((unsigned long*)addr) + (offset >> (sizeof(unsigned long) + 1));
//     unsigned long old = *p;
 
//     *p = old | mask;
 
//     return (old & mask) != 0;
// }

// void clear_bit(int offset, void *addr)
// {
//     unsigned long mask = 1UL << (offset & (sizeof(unsigned long) * BITS_PER_BYTE - 1));
//     unsigned long *p = ((unsigned long*)addr) + (offset >> (sizeof(unsigned long) + 1));
//     unsigned long old = *p;
//     *p = old & ~mask;
// }

// int find_next_zero_bit(void *addr, int size, int offset)
// {
//     unsigned long *p;
//     unsigned long mask;
 
//     while (offset < size)
//     {
//         p = ((unsigned long*)addr) + (offset >> (sizeof(unsigned long) + 1));
//         mask = 1UL << (offset & (sizeof(unsigned long) * BITS_PER_BYTE - 1));
 
//         if ((~(*p) & mask))
//         {
//             break;
//         }
//         ++offset;
//     }
 
//     return offset;
// }

// int hash(hash_node node){
//     return container_of(node, hashpid_t, node)->pid % HASHSIZE;
// }

// bool hashcmp(hash_node node1, hash_node node2){
//     return container_of(node1, hashpid_t, node)->pid == container_of(node2, hashpid_t, node)->pid;
// }

// int alloc_pid(int *last_pid, pidmap_t *pidmap)  
// {
//     int pid = *last_pid + 1;
//     int offset = pid & BITS_PER_PAGE_MASK;
    
//     if (!pidmap->nr_free)  // 通过指针访问 pidmap 的成员
//     {
//         return -1;
//     }

//     offset = find_next_zero_bit(&pidmap->page, BITS_PER_PAGE, offset);
//     if(offset == BITS_PER_PAGE) 
//         offset = find_next_zero_bit(&pidmap->page, offset - 1, 30);

//     if (BITS_PER_PAGE != offset && !test_and_set_bit(offset, &pidmap->page))
//     {
//         --pidmap->nr_free;  // 更新 pidmap->nr_free
//         *last_pid = offset;  // 更新外部的 last_pid
//         return offset;
//     }

//     return -1;
// }

// int alloc_pidmap(struct Proc* p, int *last_pid, pidmap_t *pidmap, hash_map *h)
// {
//     int pid = alloc_pid(last_pid, pidmap);  // 传递指针给 alloc_pid
//     if (pid == -1) return -1;

//     // 为 hashpid 分配内存
//     hashpid_t *hashpid = kalloc(sizeof(hashpid_t));
//     hashpid->pid = pid;
//     hashpid->proc = p;

//     // 插入到哈希表中
//     hashmap_insert(&hashpid->node, *h, hash);
//     return pid;
// }

// void free_pidmap(int pid, pidmap_t *pidmap, hash_map *h)
// {
//     int offset = pid & BITS_PER_PAGE_MASK;

//     // 释放 pid
//     if (pid > 29) pidmap->nr_free++;  // 通过指针访问 pidmap 的成员
//     clear_bit(offset, &pidmap->page);

//     // 查找并删除哈希表中的项
//     auto hashnode = hashmap_lookup(&(hashpid_t){pid, NULL, {NULL}}.node, *h, hash, hashcmp);
//     hashmap_erase(hashnode, *h, hash);

//     // 释放内存
//     kfree(container_of(hashnode, hashpid_t, node));
// }

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