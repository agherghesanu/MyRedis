#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <queue>
#include <cstdlib>
#include "heap.h"

// checks the min heap property holds at every node
static void verify_heap(const std::vector<HeapItem>& a) {
    for (size_t i = 1; i < a.size(); i++) {
        size_t parent = (i + 1) / 2 - 1;
        assert(a[parent].val <= a[i].val);        // parent never larger than child
    }
}

// checks every ref points at the slot that actually holds it
static void verify_refs(const std::vector<HeapItem>& a) {
    for (size_t i = 0; i < a.size(); i++) {
        assert(*a[i].ref == i);
    }
}

// drive random pushes and value changes then drain in sorted order
static void test_random(unsigned seed) {
    srand(seed);
    std::vector<HeapItem> a;
    std::vector<size_t> slots;                    // stable backing for ref targets
    slots.reserve(4096);

    const int N = 1000;
    for (int i = 0; i < N; i++) {
        uint64_t val = (uint64_t)(rand() % 100000);
        slots.push_back(0);
        HeapItem item = { val, &slots.back() };
        size_t pos = a.size();
        a.push_back(item);
        *a[pos].ref = pos;
        heap_update(a.data(), pos, a.size());
        verify_heap(a);
        verify_refs(a);
    }

    // randomly bump some values to new deadlines and re fix through their ref
    for (int k = 0; k < 500; k++) {
        size_t pos = (size_t)(rand() % (int)a.size());
        a[pos].val = (uint64_t)(rand() % 100000);
        heap_update(a.data(), pos, a.size());
        verify_heap(a);
        verify_refs(a);
    }

    // popping the root repeatedly must yield non decreasing values
    uint64_t prev = 0;
    while (!a.empty()) {
        uint64_t top = a[0].val;
        assert(top >= prev);                      // heap gives ascending order
        prev = top;
        // delete the root by moving the last item up then sifting
        a[0] = a.back();
        a.pop_back();
        if (!a.empty()) {
            *a[0].ref = 0;
            heap_update(a.data(), 0, a.size());
            verify_heap(a);
            verify_refs(a);
        }
    }
    printf("  test_random seed %u ok\n", seed);
}

// a deliberately worst case ascending then descending insert order
static void test_ordered() {
    std::vector<HeapItem> a;
    std::vector<size_t> slots;
    slots.reserve(400);

    auto add = [&](uint64_t val) {
        slots.push_back(0);
        HeapItem item = { val, &slots.back() };
        size_t pos = a.size();
        a.push_back(item);
        *a[pos].ref = pos;
        heap_update(a.data(), pos, a.size());
        verify_heap(a);
        verify_refs(a);
    };

    for (uint64_t v = 0; v < 100; v++) add(v);        // ascending
    for (uint64_t v = 200; v > 100; v--) add(v);      // descending
    assert(a[0].val == 0);                            // smallest floated to the root
    printf("  test_ordered ok\n");
}

int main() {
    printf("heap tests:\n");
    test_ordered();
    for (unsigned s = 1; s <= 20; s++) test_random(s);
    printf("all heap tests passed\n");
    return 0;
}
