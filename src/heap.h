#pragma once
#include <stddef.h>
#include <stdint.h>

// one slot in a binary min heap kept in a flat array
// val is the ordering key here it is an expiry deadline in milliseconds
// ref points at the owners index field so when a slot moves the owner is told
struct HeapItem {
    uint64_t val = 0;
    size_t*  ref = nullptr;
};

// restore the heap after the item at pos changed its val
// it may need to float up toward the root or sink down toward the leaves
void heap_update(HeapItem* a, size_t pos, size_t len);
