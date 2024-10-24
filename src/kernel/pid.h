#ifndef _MYHASHMAP_H
#define _MYHASHMAP_H
#include "common/defines.h"
#include <aarch64/mmu.h>
#include <kernel/mem.h>
#define HASHSIZE 128
#define PID_MAX_DEFAULT 0x8000
#define BITS_PER_BYTE 8
#define BITS_PER_PAGE (PAGE_SIZE * BITS_PER_BYTE)
#define BITS_PER_PAGE_MASK (BITS_PER_PAGE - 1)


struct hash_node_ {
    struct hash_node_* next;
};

typedef struct hash_node_ *hash_node;

struct hash_map_ {
    hash_node bullet[HASHSIZE];
};

typedef struct hashpid
{
    int pid;
    struct Proc* proc;
    struct hash_node_ node;
} hashpid_t;
//pidmap
typedef struct pidmap
{
    unsigned int nr_free;
    char page[4096];
} pidmap_t;

typedef struct hash_map_ *hash_map;
/* NOTE:You should add lock when use */
void hashmap_init(hash_map map);
int hashmap_insert(hash_node node, hash_map map, int (*hash)(hash_node node));
void hashmap_erase(hash_node node, hash_map map, int (*hash)(hash_node node));
hash_node hashmap_lookup(hash_node node, hash_map map, int (*hash)(hash_node node), bool (*hashcmp)(hash_node node1, hash_node node2));
int test_and_set_bit(int offset, void *addr);
void clear_bit(int offset, void *addr);
int find_next_zero_bit(void *addr, int size, int offset);
int hash(hash_node node);
bool hashcmp(hash_node node1, hash_node node2);
int alloc_pid(int *last_pid, pidmap_t *pidmap);
int alloc_pidmap(struct Proc* p, int *last_pid, pidmap_t *pidmap, hash_map *h);
void free_pidmap(int pid, pidmap_t *pidmap, hash_map *h);

#endif