#pragma once
#include <stddef.h>
#include <stdint.h>

// intrusive tree node, same trick as HNode: it lives inside the payload
// struct and the owner recovers itself with container_of
struct AVLNode {
    AVLNode* parent = nullptr;
    AVLNode* left = nullptr;
    AVLNode* right = nullptr;
    uint32_t height = 0;   // longest path to a leaf, drives rebalancing
    uint32_t cnt = 0;      // nodes in this subtree, drives rank queries
};

// a fresh node is a leaf, height and cnt both 1
static inline void avl_init(AVLNode* node) {
    node->left = node->right = node->parent = nullptr;
    node->height = 1;
    node->cnt = 1;
}

// null safe readers, an absent child counts as height 0 / cnt 0
static inline uint32_t avl_height(AVLNode* node) { return node ? node->height : 0; }
static inline uint32_t avl_cnt(AVLNode* node) { return node ? node->cnt : 0; }

// rebalance from node up to the root after an insert or detach
// returns the new root, since a rotation can replace it
AVLNode* avl_fix(AVLNode* node);

// unlink node from its tree, returns the new root
AVLNode* avl_del(AVLNode* node);

// jump offset positions away from node in sorted order (0 = node itself)
// negative goes left, positive goes right, NULL if it lands out of range
AVLNode* avl_offset(AVLNode* node, int64_t offset);
