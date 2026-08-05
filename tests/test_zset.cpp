#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <set>
#include <vector>
#include <utility>
#include "zset.h"

using namespace std;

// walk the whole set in sorted order by seeking to the smallest then stepping
static vector<pair<double, string>> dump(ZSet& z) {
    vector<pair<double, string>> out;
    ZNode* n = zset_seekge(&z, -INFINITY, "", 0);   // first member in order
    while (n) {
        out.push_back({ n->score, string(n->name, n->len) });
        n = znode_offset(n, +1);
    }
    return out;
}

// reference ordering is a sorted set of score name pairs
static vector<pair<double, string>> ref_dump(
        set<pair<double, string>>& ref) {
    return { ref.begin(), ref.end() };
}

static void add(ZSet& z, set<pair<double, string>>& ref,
                double score, const string& name, bool expect_new) {
    // erase any old score for this name from the reference first
    for (auto it = ref.begin(); it != ref.end(); ++it) {
        if (it->second == name) { ref.erase(it); break; }
    }
    bool added = zset_insert(&z, name.data(), name.size(), score);
    assert(added == expect_new);
    ref.insert({ score, name });
    assert(dump(z) == ref_dump(ref));           // order stays correct after every add
}

// insert distinct members and update some checking order the whole way
static void test_add_update() {
    ZSet z;
    set<pair<double, string>> ref;

    add(z, ref, 1.0, "a", true);
    add(z, ref, 2.0, "b", true);
    add(z, ref, 0.5, "c", true);
    add(z, ref, 2.0, "d", true);                // equal score tie breaks by name
    add(z, ref, 1.5, "e", true);

    add(z, ref, 5.0, "a", false);               // update existing moves it to the end
    add(z, ref, 0.1, "b", false);               // update existing moves it to the front

    // lookups return the current score
    assert(zset_lookup(&z, "a", 1)->score == 5.0);
    assert(zset_lookup(&z, "b", 1)->score == 0.1);
    assert(zset_lookup(&z, "zzz", 3) == nullptr);

    zset_clear(&z);
    printf("  test_add_update ok\n");
}

// seekge must land on the first member at or after the query pair
static void test_seek() {
    ZSet z;
    for (int i = 0; i < 10; i++) {              // even scores up to 18 names k0 through k9
        string name = "k" + to_string(i);
        zset_insert(&z, name.data(), name.size(), i * 2.0);
    }

    ZNode* n = zset_seekge(&z, 6.0, "", 0);     // exact score present
    assert(n && n->score == 6.0);

    n = zset_seekge(&z, 5.0, "", 0);            // between two scores rounds up
    assert(n && n->score == 6.0);

    n = zset_seekge(&z, 100.0, "", 0);          // past the end
    assert(n == nullptr);

    n = zset_seekge(&z, -1.0, "", 0);           // before the start
    assert(n && n->score == 0.0);

    zset_clear(&z);
    printf("  test_seek ok\n");
}

// znode_offset gives rank movement forward back and out of range
static void test_offset() {
    ZSet z;
    for (int i = 0; i < 6; i++) {               // scores 0 through 5 names a through f
        string name(1, char('a' + i));
        zset_insert(&z, name.data(), name.size(), i);
    }

    ZNode* first = zset_seekge(&z, -INFINITY, "", 0);
    assert(first && first->score == 0.0);

    ZNode* third = znode_offset(first, 2);      // forward two ranks
    assert(third && third->score == 2.0);

    ZNode* back = znode_offset(third, -1);      // step back one rank
    assert(back && back->score == 1.0);

    assert(znode_offset(first, 6) == nullptr);  // one past the last
    assert(znode_offset(first, -1) == nullptr); // before the first

    zset_clear(&z);
    printf("  test_offset ok\n");
}

// delete removes from both the tree order and the name lookup
static void test_delete() {
    ZSet z;
    set<pair<double, string>> ref;
    for (int i = 0; i < 20; i++) {
        string name = "m" + to_string(i);
        zset_insert(&z, name.data(), name.size(), i);
        ref.insert({ (double)i, name });
    }

    for (int i = 0; i < 20; i += 2) {           // delete every even ranked member
        string name = "m" + to_string(i);
        ZNode* node = zset_lookup(&z, name.data(), name.size());
        assert(node);
        zset_delete(&z, node);
        ref.erase({ (double)i, name });
        assert(zset_lookup(&z, name.data(), name.size()) == nullptr);
        assert(dump(z) == ref_dump(ref));       // order stays correct after each delete
    }

    zset_clear(&z);
    printf("  test_delete ok\n");
}

int main() {
    printf("zset tests:\n");
    test_add_update();
    test_seek();
    test_offset();
    test_delete();
    printf("all zset tests passed\n");
    return 0;
}
