#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "global.h"
#include "hashtab.h"

#define hash_func meiyan

static inline uint32_t meiyan(const char *key, int count) {
	typedef uint32_t *P;
	uint32_t h = 0x811c9dc5;
	while (count >= 8) {
		h = (h ^ ((((*(P)key) << 5) | ((*(P)key) >> 27)) ^ *(P)(key + 4))) * 0xad3e7;
		count -= 8;
		key += 8;
	}
	#define tmp h = (h ^ *(uint16_t*)key) * 0xad3e7; key += 2;
	if (count & 4) { tmp tmp }
	if (count & 2) { tmp }
	if (count & 1) { h = (h ^ *key) * 0xad3e7; }
	#undef tmp
	return h ^ (h >> 16);
}

struct hashtab *ht_new(char *whoami, unsigned int value_size, unsigned int nbuckets,
        unsigned int nworkers, bool align16) {
    struct hashtab *ht = new_alloc(struct hashtab);
    ht->align16 = align16;
    ht->value_size = value_size;
	if (nbuckets == 0) {
        nbuckets = 1024;
    }
    ht->nbuckets = nbuckets;
    ht->buckets = malloc(nbuckets * sizeof(_Atomic(struct ht_node *)));
    for (unsigned int i = 0; i < nbuckets; i++) {
        atomic_init(&ht->buckets[i], NULL);
    }
    atomic_init(&ht->nobjects, 0);
    return ht;
}

void ht_resize(struct hashtab *ht, unsigned int nbuckets){
    _Atomic(struct ht_node *) *old_buckets = ht->buckets;
    unsigned int old_nbuckets = ht->nbuckets;
    ht->nbuckets = nbuckets;
    ht->buckets = malloc(nbuckets * sizeof(_Atomic(struct ht_node *)));
    for (unsigned int i = 0; i < nbuckets; i++) {
        atomic_init(&ht->buckets[i], NULL);
    }
    for (unsigned int i = 0; i < old_nbuckets; i++) {
        struct ht_node *n = atomic_load(&old_buckets[i]), *next;
        for (; n != NULL; n = next) {
            assert(n->size == sizeof(uint32_t));
            next = atomic_load(&n->next);
            unsigned int hash = hash_func((char *) &n[1], n->size) % nbuckets;
            // printf("MOV %u %u -> %u\n", * (uint32_t *) &n[1], i, hash);
            atomic_store(&n->next, atomic_load(&ht->buckets[hash]));
            atomic_store(&ht->buckets[hash], n);
        }
    }
    free(old_buckets);
}

struct ht_node *ht_find(struct hashtab *ht, struct allocator *al, const void *key, unsigned int size, bool *is_new){
    unsigned int hash = hash_func(key, size) % ht->nbuckets;

    // First do a search
    _Atomic(struct ht_node *) *chain = &ht->buckets[hash];
    for (;;) {
        struct ht_node *expected = atomic_load(chain);
        if (expected == NULL) {
            break;
        }
        if (expected->size == size && memcmp(&expected[1], key, size) == 0) {
            if (is_new != NULL) {
                *is_new = false;
            }
            return expected;
        }
        chain = &expected->next;
    }

    // Allocated a new node
    unsigned int total = sizeof(struct ht_node) + size;
	struct ht_node *desired = al == NULL ?
            malloc(total) : (*al->alloc)(al->ctx, total, false, ht->align16);
    atomic_init(&desired->next, NULL);
    desired->size = size;
    memcpy(&desired[1], key, size);

    // Insert the node
    for (;;) {
        struct ht_node *expected = NULL;
        if (atomic_compare_exchange_strong(chain, &expected, desired)) {
            atomic_fetch_add(&ht->nobjects, 1);
            if (is_new != NULL) {
                *is_new = true;
            }
            return desired;
        }
        else if (expected->size == size && memcmp(&expected[1], key, size) == 0) {
            free(desired);      // somebody else beat me to it
            if (is_new != NULL) {
                *is_new = false;
            }
            return expected;
        }
        chain = &expected->next;
    }
}

void *ht_retrieve(struct ht_node *n, unsigned int *psize){
    if (psize != NULL) {
        *psize = n->size;
    }
    return &n[1];
}
