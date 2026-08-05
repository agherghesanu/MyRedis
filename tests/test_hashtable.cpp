#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <set>
#include "hashtable.h"

using namespace std;

// go from an embedded node back to its owning payload
#define container_of(ptr, T, member) \
    ((T *)( (char *)ptr - offsetof(T, member) ))

// payload wrapping an HNode, key doubles as the hashed value
struct Item {
    HNode node;
    uint32_t key = 0;
};

// key equality injected into the table, which only sees HNode
static bool item_eq(HNode* a, HNode* b) {
    return container_of(a, Item, node)->key == container_of(b, Item, node)->key;
}

// trivial hash, mixes the bits a little so buckets spread out
static uint64_t int_hash(uint32_t key) {
    uint64_t h = key;
    h = (h + 0x9E3779B9) * 0x85EBCA6B;
    return h;
}

// build a stack probe for lookup/delete, never inserted
static Item make_probe(uint32_t key) {
    Item probe;
    probe.key = key;
    probe.node.hcode = int_hash(key);
    return probe;
}

// insert one key, caller owns the heap Item until deleted
static void insert(HMap* m, uint32_t key) {
    Item* it = new Item();
    it->key = key;
    it->node.hcode = int_hash(key);
    hm_insert(m, &it->node);
}

// true if key is currently in the table
static bool contains(HMap* m, uint32_t key) {
    Item probe = make_probe(key);
    return hm_lookup(m, &probe.node, &item_eq) != nullptr;
}

// free every payload, then release the bucket arrays
static void destroy(HMap* m) {
    // hm_foreach hands each node to a lambda-free callback via a captureless fn
    hm_foreach(m, [](HNode* n, void*) {
        delete container_of(n, Item, node);
    }, nullptr);
    hm_clear(m);
}

// empty map: lookup misses, size is zero, delete of absent key is NULL
static void test_empty() {
    HMap m;
    assert(hm_size(&m) == 0);
    assert(!contains(&m, 42));
    Item probe = make_probe(42);
    assert(hm_delete(&m, &probe.node, &item_eq) == nullptr);
    hm_clear(&m);
    printf("  test_empty ok\n");
}

// single key round trip: insert, find, delete, gone
static void test_single() {
    HMap m;
    insert(&m, 7);
    assert(hm_size(&m) == 1);
    assert(contains(&m, 7));
    assert(!contains(&m, 8));

    Item probe = make_probe(7);
    HNode* removed = hm_delete(&m, &probe.node, &item_eq);
    assert(removed);
    delete container_of(removed, Item, node);

    assert(hm_size(&m) == 0);
    assert(!contains(&m, 7));
    hm_clear(&m);
    printf("  test_single ok\n");
}

// many keys forces a resize; every key must stay findable across the migration
static void test_resize_and_lookup() {
    HMap m;
    const uint32_t N = 5000;                 // well past load factor, several resizes
    for (uint32_t i = 0; i < N; i++) insert(&m, i);
    assert(hm_size(&m) == N);

    for (uint32_t i = 0; i < N; i++) assert(contains(&m, i));   // all present
    assert(!contains(&m, N));                                   // one past is absent
    assert(!contains(&m, N + 12345));

    destroy(&m);
    printf("  test_resize_and_lookup ok\n");
}

// interleave inserts and deletes so lookups run while a migration is in flight
static void test_delete_during_migration() {
    HMap m;
    const uint32_t N = 4000;
    for (uint32_t i = 0; i < N; i++) insert(&m, i);

    // delete every even key, checking survivors after each removal
    uint32_t expected = N;
    for (uint32_t i = 0; i < N; i += 2) {
        Item probe = make_probe(i);
        HNode* removed = hm_delete(&m, &probe.node, &item_eq);
        assert(removed);
        delete container_of(removed, Item, node);
        expected--;
        assert(hm_size(&m) == expected);
    }

    for (uint32_t i = 0; i < N; i++) {
        assert(contains(&m, i) == (i % 2 == 1));   // odds survive, evens gone
    }

    destroy(&m);
    printf("  test_delete_during_migration ok\n");
}

// second delete of the same key returns NULL, size unchanged
static void test_double_delete() {
    HMap m;
    insert(&m, 100);
    Item probe = make_probe(100);
    HNode* first = hm_delete(&m, &probe.node, &item_eq);
    assert(first);
    delete container_of(first, Item, node);
    assert(hm_delete(&m, &probe.node, &item_eq) == nullptr);
    assert(hm_size(&m) == 0);
    hm_clear(&m);
    printf("  test_double_delete ok\n");
}

// hm_foreach must visit every key exactly once, even mid-migration
static void test_foreach() {
    HMap m;
    const uint32_t N = 2000;
    for (uint32_t i = 0; i < N; i++) insert(&m, i);

    set<uint32_t> seen;
    hm_foreach(&m, [](HNode* n, void* arg) {
        auto* s = (set<uint32_t>*)arg;
        bool inserted = s->insert(container_of(n, Item, node)->key).second;
        assert(inserted);                    // no key visited twice
    }, &seen);
    assert(seen.size() == N);

    destroy(&m);
    printf("  test_foreach ok\n");
}

int main() {
    printf("hashtable tests:\n");
    test_empty();
    test_single();
    test_resize_and_lookup();
    test_delete_during_migration();
    test_double_delete();
    test_foreach();
    printf("all hashtable tests passed\n");
    return 0;
}
