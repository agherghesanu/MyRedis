#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "zset.h"

// go from an embedded node back to its owning payload
#define container_of(ptr, T, member) \
    ((T *)( (char *)ptr - offsetof(T, member) ))

// same fnv hash the server uses duplicated here so zset owns its own indexing
static uint64_t str_hash(const uint8_t* data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++) {
        h = (h + data[i]) * 0x01000193;
    }
    return h;
}

// shorter of two lengths for a bounded memcmp
static size_t min_sz(size_t a, size_t b) { return a < b ? a : b; }

// order a tree node against a raw score name pair
// score wins first then name bytes then the shorter name sorts earlier
static bool zless(AVLNode* lhs, double score, const char* name, size_t len) {
    ZNode* zl = container_of(lhs, ZNode, tree);
    if (zl->score != score) return zl->score < score;
    int rv = memcmp(zl->name, name, min_sz(zl->len, len));
    if (rv != 0) return rv < 0;
    return zl->len < len;
}

// order two tree nodes by unpacking the right one into the pair form
static bool zless(AVLNode* lhs, AVLNode* rhs) {
    ZNode* zr = container_of(rhs, ZNode, tree);
    return zless(lhs, zr->score, zr->name, zr->len);
}

// allocate a member with its name bytes trailing the struct in one block
static ZNode* znode_new(const char* name, size_t len, double score) {
    ZNode* node = (ZNode*)malloc(sizeof(ZNode) + len);
    assert(node);
    avl_init(&node->tree);
    node->hmap.next = nullptr;
    node->hmap.hcode = str_hash((const uint8_t*)name, len);
    node->score = score;
    node->len = len;
    memcpy(&node->name[0], name, len);
    return node;
}

// bst descent using the comparator then rebalance up to the root
static void tree_insert(ZSet* zset, ZNode* node) {
    AVLNode* parent = nullptr;
    AVLNode** from = &zset->root;
    while (*from) {
        parent = *from;
        from = zless(&node->tree, parent) ? &parent->left : &parent->right;
    }
    *from = &node->tree;
    node->tree.parent = parent;
    zset->root = avl_fix(&node->tree);
}

// score change moves the member so detach from the tree and reinsert
// the hmap is untouched since the name is the same
static void zset_update(ZSet* zset, ZNode* node, double score) {
    if (node->score == score) return;
    zset->root = avl_del(&node->tree);
    avl_init(&node->tree);
    node->score = score;
    tree_insert(zset, node);
}

bool zset_insert(ZSet* zset, const char* name, size_t len, double score) {
    ZNode* node = zset_lookup(zset, name, len);
    if (node) {
        zset_update(zset, node, score);
        return false;
    }
    node = znode_new(name, len, score);
    hm_insert(&zset->hmap, &node->hmap);
    tree_insert(zset, node);
    return true;
}

// probe carried into the hmap lookup so eq can see the raw name
struct HKey {
    HNode node;
    const char* name = nullptr;
    size_t len = 0;
};

// name equality injected into the hmap which only sees hnode
static bool hcmp(HNode* node, HNode* key) {
    ZNode* znode = container_of(node, ZNode, hmap);
    HKey* hkey = container_of(key, HKey, node);
    if (znode->len != hkey->len) return false;
    return 0 == memcmp(znode->name, hkey->name, znode->len);
}

ZNode* zset_lookup(ZSet* zset, const char* name, size_t len) {
    if (!zset->root) return nullptr;
    HKey key;
    key.node.hcode = str_hash((const uint8_t*)name, len);
    key.name = name;
    key.len = len;
    HNode* found = hm_lookup(&zset->hmap, &key.node, &hcmp);
    return found ? container_of(found, ZNode, hmap) : nullptr;
}

void zset_delete(ZSet* zset, ZNode* node) {
    // drop from the hmap using a probe built from the node itself
    HKey key;
    key.node.hcode = node->hmap.hcode;
    key.name = node->name;
    key.len = node->len;
    HNode* removed = hm_delete(&zset->hmap, &key.node, &hcmp);
    assert(removed);
    // drop from the tree then free the one block
    zset->root = avl_del(&node->tree);
    free(node);
}

ZNode* zset_seekge(ZSet* zset, double score, const char* name, size_t len) {
    AVLNode* found = nullptr;
    for (AVLNode* node = zset->root; node; ) {
        if (zless(node, score, name, len)) {
            node = node->right;     // node is below the key so go right
        }
        else {
            found = node;           // node is a candidate keep the leftmost one
            node = node->left;
        }
    }
    return found ? container_of(found, ZNode, tree) : nullptr;
}

ZNode* znode_offset(ZNode* node, int64_t offset) {
    AVLNode* tnode = node ? avl_offset(&node->tree, offset) : nullptr;
    return tnode ? container_of(tnode, ZNode, tree) : nullptr;
}

// post order walk so children are freed before their parent
static void tree_dispose(AVLNode* node) {
    if (!node) return;
    tree_dispose(node->left);
    tree_dispose(node->right);
    free(container_of(node, ZNode, tree));
}

void zset_clear(ZSet* zset) {
    hm_clear(&zset->hmap);          // releases the bucket arrays only
    tree_dispose(zset->root);       // frees the actual members
    zset->root = nullptr;
}
