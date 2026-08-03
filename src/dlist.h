#pragma once

// intrusive circular doubly linked list node
// same idea as hnode and avlnode it lives inside the payload struct
// a list head is just an empty node whose next and prev point at itself
struct DList {
    DList* prev = nullptr;
    DList* next = nullptr;
};

// make a node point at itself which is both an empty head and a detached node
static inline void dlist_init(DList* node) {
    node->prev = node->next = node;
}

// a head is empty when it still points only at itself
static inline bool dlist_empty(DList* node) {
    return node->next == node;
}

// unlink a node by stitching its neighbours together
static inline void dlist_detach(DList* node) {
    DList* prev = node->prev;
    DList* next = node->next;
    prev->next = next;
    next->prev = prev;
}

// splice rookie in just ahead of target
// inserting before the head means appending to the back of the list
static inline void dlist_insert_before(DList* target, DList* rookie) {
    DList* prev = target->prev;
    prev->next = rookie;
    rookie->prev = prev;
    rookie->next = target;
    target->prev = rookie;
}
