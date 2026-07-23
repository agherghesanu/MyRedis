#pragma once
#include <stddef.h>
#include <stdint.h>

// lives inside the payload. get with container_of
struct HNode {
    HNode* next = nullptr;
    uint64_t hcode = 0;   // cached hash cheap reject before calling eq
};

// one bucket array. 
struct HTab {
    HNode** tab = nullptr;   // not allocated
    size_t  mask = 0;         // capacity - 1 2 pow though
    size_t  size = 0;
};

// two tables during a resize, migration happens over a longer period of time
struct HMap {
    HTab   newer;             // inserts always go here
    HTab   older;             // draining 
    size_t migrate_pos = 0;
};

HNode* hm_lookup(HMap*, HNode* key, bool (*eq)(HNode*, HNode*));
void   hm_insert(HMap*, HNode* node);
HNode* hm_delete(HMap*, HNode* key, bool (*eq)(HNode*, HNode*));
size_t hm_size(HMap*);
void   hm_foreach(HMap*, void (*f)(HNode*, void*), void* arg);
void   hm_clear(HMap*);       // frees arrays only drain entries first