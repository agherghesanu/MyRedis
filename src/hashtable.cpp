#include <assert.h>
#include <stdlib.h>
#include "hashtable.h"

static void h_init(HTab* t, size_t n) {
    assert(n > 0 && ((n - 1) & n) == 0);       // power of 2
    t->tab = (HNode**)calloc(n, sizeof(HNode*));
    assert(t->tab);
    t->mask = n - 1;
    t->size = 0;
}

// prepend to the chain order within a bucket dont matter
static void h_insert(HTab* t, HNode* node) {
    size_t pos = node->hcode & t->mask;
    node->next = t->tab[pos];
    t->tab[pos] = node;
    t->size++;
}

// returns the address of the link pointing to the match not the node.
static HNode** h_lookup(HTab* t, HNode* key, bool (*eq)(HNode*, HNode*)) {
    if (!t->tab) return NULL;
    HNode** from = &t->tab[key->hcode & t->mask];
    for (HNode* cur; (cur = *from) != NULL; from = &cur->next) {
        if (cur->hcode == key->hcode && eq(cur, key)) return from;
    }
    return NULL;
}

static HNode* h_detach(HTab* t, HNode** from) {
    HNode* node = *from;
    *from = node->next;
    t->size--;
    return node;
}

const size_t k_rehashing_work = 128;    // nodes migrated per operation
const size_t k_max_load_factor = 8;     // chain length before doubling



//moves a bounded batch, frees older when empty -> big resize less problem
static void hm_help_rehashing(HMap* m) {
    size_t nwork = 0;
    while (nwork < k_rehashing_work && m->older.size > 0
        && m->migrate_pos <= m->older.mask) {   // bound dont run off the array
        HNode** from = &m->older.tab[m->migrate_pos];
        if (!*from) { m->migrate_pos++; continue; }  // empty bucket is free
        h_insert(&m->newer, h_detach(&m->older, from));
        nwork++;
    }
    if (m->older.size == 0 && m->older.tab) {
        free(m->older.tab);
        m->older = HTab{};      // tab = NULL is the donr flag
    }
}

// only two tables exist so a second trigger mid-migration would lose entries.
static void hm_trigger_rehashing(HMap* m) {
    assert(m->older.tab == NULL);
    m->older = m->newer;
    h_init(&m->newer, (m->newer.mask + 1) * 2);
    m->migrate_pos = 0;
}

// both talbe a preresize key may not have migrated yet Newer hits sooner.
HNode* hm_lookup(HMap* m, HNode* key, bool (*eq)(HNode*, HNode*)) {
    hm_help_rehashing(m);
    HNode** from = h_lookup(&m->newer, key, eq);
    if (!from) from = h_lookup(&m->older, key, eq);
    return from ? *from : NULL;
}

void hm_insert(HMap* m, HNode* node) {
    if (!m->newer.tab) h_init(&m->newer, 4);   // lazy empty map allocates nothing
    h_insert(&m->newer, node);
    if (!m->older.tab) {                       // dont stack resizes
        if (m->newer.size >= (m->newer.mask + 1) * k_max_load_factor)
            hm_trigger_rehashing(m);
    }
    hm_help_rehashing(m);
}

// caller owns the returned node and frees the payload around it
HNode* hm_delete(HMap* m, HNode* key, bool (*eq)(HNode*, HNode*)) {
    hm_help_rehashing(m);
    if (HNode** from = h_lookup(&m->newer, key, eq)) {
        return h_detach(&m->newer, from);
    }
    if (HNode** from = h_lookup(&m->older, key, eq)) {
        return h_detach(&m->older, from);
    }
    return NULL;
}

// caches next first, so f may free the node 
static void h_foreach(HTab* t, void (*f)(HNode*, void*), void* arg) {
    if (!t->tab) return;
    for (size_t i = 0; i <= t->mask; i++) {
        for (HNode* n = t->tab[i]; n; ) { 
            HNode* next = n->next; 
            f(n, arg); 
            n = next; 
        }
    }
}

void hm_foreach(HMap* m, void (*f)(HNode*, void*), void* arg) {
    h_foreach(&m->newer, f, arg);
    h_foreach(&m->older, f, arg);
}

// Frees the arrays, not the entries — intrusive nodes belong to payload
// structs this file can't name. hm_foreach a destructor first or they leak.

//frees the arrays not the entries. instruse nodes belon to the paload struct
void hm_clear(HMap* m) {
    free(m->newer.tab);
    free(m->older.tab);
    *m = HMap{};
}

// split across the table mid rehash so sum is the size
size_t hm_size(HMap* m) {
    return m->newer.size + m->older.size;
}