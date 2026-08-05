#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <vector>
#include "avl.h"

using namespace std;

// same container_of trick the server uses to go from node back to payload
#define container_of(ptr, T, member) \
    ((T *)( (char *)ptr - offsetof(T, member) ))

// payload wrapping an AVLNode, val is the sort key
struct Data {
    AVLNode node;
    uint32_t val = 0;
};

// holds the tree root plus a shadow multiset to check answers against
struct Container {
    AVLNode* root = nullptr;
};

// plain bst descent to find the slot, then avl_fix rebalances up to the root
static void add(Container& c, uint32_t val) {
    Data* data = new Data();
    avl_init(&data->node);
    data->val = val;

    AVLNode* cur = nullptr;
    AVLNode** from = &c.root;             // incoming link to fill
    while (*from) {
        cur = *from;
        uint32_t node_val = container_of(cur, Data, node)->val;
        from = (val < node_val) ? &cur->left : &cur->right;
    }
    *from = &data->node;
    data->node.parent = cur;
    c.root = avl_fix(&data->node);
}

// find by value, avl_del splices it out, then free the payload
static bool del(Container& c, uint32_t val) {
    AVLNode* cur = c.root;
    while (cur) {
        uint32_t node_val = container_of(cur, Data, node)->val;
        if (val == node_val) break;
        cur = (val < node_val) ? cur->left : cur->right;
    }
    if (!cur) return false;
    c.root = avl_del(cur);
    delete container_of(cur, Data, node);
    return true;
}

// recompute from raw links so we can catch a stale cached field
static uint32_t real_height(AVLNode* n) {
    if (!n) return 0;
    uint32_t l = real_height(n->left);
    uint32_t r = real_height(n->right);
    return 1 + (l > r ? l : r);
}
static uint32_t real_cnt(AVLNode* n) {
    return n ? 1 + real_cnt(n->left) + real_cnt(n->right) : 0;
}

// assert every avl invariant at every node
static void verify(AVLNode* parent, AVLNode* node) {
    if (!node) return;
    assert(node->parent == parent);
    verify(node, node->left);
    verify(node, node->right);

    assert(node->cnt == real_cnt(node));                 // cached cnt is honest
    assert(node->height == real_height(node));           // cached height is honest

    uint32_t l = avl_height(node->left);
    uint32_t r = avl_height(node->right);
    assert(l == r || l + 1 == r || l == r + 1);          // balanced within 1

    uint32_t val = container_of(node, Data, node)->val;  // bst order holds
    if (node->left)  assert(container_of(node->left, Data, node)->val <= val);
    if (node->right) assert(container_of(node->right, Data, node)->val >= val);
}

// flatten the tree in sorted order for comparison against the shadow set
static void extract(AVLNode* node, vector<uint32_t>& out) {
    if (!node) return;
    extract(node->left, out);
    out.push_back(container_of(node, Data, node)->val);
    extract(node->right, out);
}

// build a tree of size sz, verifying invariants after every single insert
static void test_insert(uint32_t sz) {
    for (uint32_t start = 0; start < sz; start++) {      // rotate the insert order
        Container c;
        multiset<uint32_t> ref;
        for (uint32_t i = 0; i < sz; i++) {
            uint32_t val = (start + i) % sz;
            add(c, val);
            ref.insert(val);
            verify(nullptr, c.root);
        }
        vector<uint32_t> got;
        extract(c.root, got);
        assert(got.size() == ref.size());
        assert(vector<uint32_t>(ref.begin(), ref.end()) == got);  // sorted match

        // tear down, verifying after each delete too
        while (c.root) {
            uint32_t val = container_of(c.root, Data, node)->val;
            assert(del(c, val));
            verify(nullptr, c.root);
        }
    }
}

// avl_offset(kth) must equal the kth smallest value
static void test_offset(uint32_t sz) {
    Container c;
    for (uint32_t i = 0; i < sz; i++) add(c, i);          // values 0..sz-1

    AVLNode* min = c.root;
    while (min->left) min = min->left;                    // leftmost = rank 0
    for (uint32_t rank = 0; rank < sz; rank++) {
        AVLNode* n = avl_offset(min, (int64_t)rank);
        assert(n && container_of(n, Data, node)->val == rank);
    }
    assert(avl_offset(min, (int64_t)sz) == nullptr);      // one past the end
}

int main() {
    for (uint32_t sz = 1; sz <= 200; sz++) {
        test_insert(sz);
    }
    test_offset(1000);
    printf("all avl tests passed\n");
    return 0;
}
