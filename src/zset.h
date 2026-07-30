#pragma once
#include <stddef.h>
#include <stdint.h>
#include "avl.h"
#include "hashtable.h"

// a sorted set holds every member twice at once
// the tree keeps them ordered by score then name for range and rank queries
// the hmap gives o1 lookup of a member by its name
struct ZSet {
    AVLNode* root = nullptr;
    HMap hmap;
};

// one member of a sorted set
// tree and hmap are the two intrusive nodes that thread it into both indexes
// name is stored inline right after the struct so a member is one allocation
struct ZNode {
    AVLNode tree;
    HNode   hmap;
    double  score = 0;
    size_t  len = 0;
    char    name[0];
};

// add name with score or move an existing name to a new score
// returns true when the member was newly created
bool   zset_insert(ZSet* zset, const char* name, size_t len, double score);

// find a member by name or null when absent
ZNode* zset_lookup(ZSet* zset, const char* name, size_t len);

// unlink a member from both indexes and free it
void   zset_delete(ZSet* zset, ZNode* node);

// first member at or after the pair score name in sorted order
ZNode* zset_seekge(ZSet* zset, double score, const char* name, size_t len);

// step offset positions from node in sorted order null when out of range
ZNode* znode_offset(ZNode* node, int64_t offset);

// free every member and reset the set to empty
void   zset_clear(ZSet* zset);
