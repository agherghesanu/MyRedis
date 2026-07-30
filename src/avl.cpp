#include <assert.h>
#include "avl.h"

static uint32_t max(uint32_t a, uint32_t b) { return a > b ? a : b; }

// recompute height and cnt from the two children, call after any link change
static void avl_update(AVLNode* node) {
    node->height = 1 + max(avl_height(node->left), avl_height(node->right));
    node->cnt = 1 + avl_cnt(node->left) + avl_cnt(node->right);
}

// pivot node's right child up, node becomes its left child
// the pivot's old left subtree (inner) reattaches under node
static AVLNode* rot_left(AVLNode* node) {
    AVLNode* parent = node->parent;
    AVLNode* new_node = node->right;
    AVLNode* inner = new_node->left;

    node->right = inner;                 // inner moves under node
    if (inner) inner->parent = node;

    new_node->left = node;               // node drops under new_node
    node->parent = new_node;
    new_node->parent = parent;           // new_node takes node's old slot

    avl_update(node);                    // children first, then parent
    avl_update(new_node);
    return new_node;
}

// mirror of rot_left
static AVLNode* rot_right(AVLNode* node) {
    AVLNode* parent = node->parent;
    AVLNode* new_node = node->left;
    AVLNode* inner = new_node->right;

    node->left = inner;
    if (inner) inner->parent = node;

    new_node->right = node;
    node->parent = new_node;
    new_node->parent = parent;

    avl_update(node);
    avl_update(new_node);
    return new_node;
}

// left subtree too tall by 2, pick single vs double rotation by the inner shape
static AVLNode* avl_fix_left(AVLNode* node) {
    if (avl_height(node->left->left) < avl_height(node->left->right)) {
        node->left = rot_left(node->left);   // left-right case, straighten first
    }
    return rot_right(node);
}

// mirror of avl_fix_left
static AVLNode* avl_fix_right(AVLNode* node) {
    if (avl_height(node->right->right) < avl_height(node->right->left)) {
        node->right = rot_right(node->right);
    }
    return rot_left(node);
}

// walk up from node, updating aux data and rotating wherever it went off
// balance, invariant is |left height - right height| <= 1 at every node
AVLNode* avl_fix(AVLNode* node) {
    while (true) {
        AVLNode** from = &node;              // where the (maybe rotated) subtree hangs
        AVLNode* parent = node->parent;
        if (parent) {
            from = parent->left == node ? &parent->left : &parent->right;
        }
        avl_update(node);

        uint32_t l = avl_height(node->left);
        uint32_t r = avl_height(node->right);
        if (l == r + 2) {
            *from = avl_fix_left(node);
        }
        else if (r == l + 2) {
            *from = avl_fix_right(node);
        }

        if (!parent) return *from;           // reached the root
        node = parent;                       // keep climbing
    }
}

// detach a node with at most one child, trivial: child slides up into its slot
static AVLNode* avl_del_easy(AVLNode* node) {
    assert(!node->left || !node->right);     // caller guarantees one child max
    AVLNode* child = node->left ? node->left : node->right;
    AVLNode* parent = node->parent;

    if (child) child->parent = parent;       // child adopts node's parent
    if (!parent) return child;               // node was the root

    AVLNode** from = parent->left == node ? &parent->left : &parent->right;
    *from = child;
    return avl_fix(parent);                  // parent's height may have dropped
}

// unlink any node, returns the new root
AVLNode* avl_del(AVLNode* node) {
    if (!node->left || !node->right) {
        return avl_del_easy(node);           // easy case handles it directly
    }

    // two children: swap node with its in-order successor (leftmost on the right)
    // the successor has no left child, so removing it is always the easy case
    AVLNode* victim = node->right;
    while (victim->left) victim = victim->left;
    AVLNode* root = avl_del_easy(victim);    // remove successor from its spot

    *victim = *node;                         // successor takes over node's links + aux
    if (victim->left) victim->left->parent = victim;
    if (victim->right) victim->right->parent = victim;

    AVLNode** from = &root;                  // splice victim into node's old slot
    AVLNode* parent = node->parent;
    if (parent) {
        from = parent->left == node ? &parent->left : &parent->right;
    }
    *from = victim;
    return root;
}

// seek to the node offset positions away in sorted order using subtree counts
// walks down when the target is inside a child subtree, up otherwise, O(log n)
AVLNode* avl_offset(AVLNode* node, int64_t offset) {
    int64_t pos = 0;                         // position of node relative to the start
    while (offset != pos) {
        if (pos < offset && pos + avl_cnt(node->right) >= offset) {
            node = node->right;              // target sits in the right subtree
            pos += avl_cnt(node->left) + 1;
        }
        else if (pos > offset && pos - avl_cnt(node->left) <= offset) {
            node = node->left;               // target sits in the left subtree
            pos -= avl_cnt(node->right) + 1;
        }
        else {
            AVLNode* parent = node->parent;  // target is outside, climb up
            if (!parent) return nullptr;     // ran off the tree
            if (parent->right == node) {
                pos -= avl_cnt(node->left) + 1;
            }
            else {
                pos += avl_cnt(node->right) + 1;
            }
            node = parent;
        }
    }
    return node;
}
