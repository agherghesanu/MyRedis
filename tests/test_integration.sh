#!/usr/bin/env bash
# end to end test starts a real redis_server drives it with redis_client over
# the socket and asserts the clients printed output covers the full path
# request framing then parse then dispatch then serialize then client parse
#
# usage test_integration.sh server_binary client_binary
# binaries default to redis_server and redis_client in the cwd
set -u

SERVER="${1:-./redis_server}"
CLIENT="${2:-./redis_client}"

pass=0
fail=0

# run the client and compare its full stdout against an expected block exactly
# args are description then expected output then the client args
expect() {
    local desc="$1"; shift
    local want="$1"; shift
    local got
    got="$("$CLIENT" "$@" 2>&1)"
    if [[ "$got" == "$want" ]]; then
        echo "  ok: $desc"
        pass=$((pass + 1))
    else
        echo "  FAIL: $desc"
        echo "    args:     $*"
        echo "    expected: [$want]"
        echo "    got:      [$got]"
        fail=$((fail + 1))
    fi
}

# start a fresh server so the keyspace begins empty
"$SERVER" &
SRV=$!
trap 'kill "$SRV" 2>/dev/null' EXIT

# wait for the listener to accept up to about two seconds
for _ in $(seq 1 20); do
    "$CLIENT" get __probe__ >/dev/null 2>&1 && break
    sleep 0.1
done

echo "integration tests:"

# basic get and set on an empty keyspace
expect "get on empty keyspace -> nil"   "(nil)"        get k
expect "set returns nil"                "(nil)"        set k v1
expect "get returns the value"          "(str) v1"     get k

# overwrite
expect "overwrite same key"             "(nil)"        set k v2
expect "get sees new value"             "(str) v2"     get k

# empty value is a string distinct from a missing key which is nil
expect "set empty string value"         "(nil)"        set e ""
expect "empty value reads as empty str" "(str) "       get e
expect "missing key is nil not empty"   "(nil)"        get nope

# keys order is unspecified so sort the str lines before comparing
# expected keyspace now holds k and e
keys_out="$("$CLIENT" keys 2>&1)"
keys_sorted="$(printf '%s\n' "$keys_out" | sort)"
keys_want="$(printf '%s\n' '(arr) end' '(arr) len=2' '(str) e' '(str) k' | sort)"
if [[ "$keys_sorted" == "$keys_want" ]]; then
    echo "  ok: keys lists all entries"; pass=$((pass + 1))
else
    echo "  FAIL: keys lists all entries"
    echo "    expected(sorted): [$keys_want]"
    echo "    got(sorted):      [$keys_sorted]"
    fail=$((fail + 1))
fi

# del reports how many were removed
expect "del present key -> 1"           "(int) 1"      del k
expect "del again -> 0"                 "(int) 0"      del k
expect "get after del -> nil"           "(nil)"        get k
expect "del other key -> 1"             "(int) 1"      del e
expect "keys empty after deletes"       "$(printf '%s\n' '(arr) len=0' '(arr) end')"  keys

# sorted set zadd reports new versus updated
expect "zadd new member -> 1"           "(int) 1"      zadd z 1 alice
expect "zadd another member -> 1"       "(int) 1"      zadd z 3 bob
expect "zadd third member -> 1"         "(int) 1"      zadd z 2 carol
expect "zadd existing member -> 0"      "(int) 0"      zadd z 5 alice

# zscore reads back the current score nil when absent
expect "zscore existing"                "(dbl) 2"      zscore z carol
expect "zscore updated member"          "(dbl) 5"      zscore z alice
expect "zscore missing member -> nil"   "(nil)"        zscore z nobody

# zquery walks in ascending score order flat name score pairs
# current set bob 3 carol 2 alice 5 so order is carol 2 then bob 3 then alice 5
expect "zquery from start limit 10" \
"$(printf '%s\n' '(arr) len=6' '(str) carol' '(dbl) 2' '(str) bob' '(dbl) 3' '(str) alice' '(dbl) 5' '(arr) end')" \
    zquery z -inf "" 0 10

expect "zquery with offset skips members" \
"$(printf '%s\n' '(arr) len=4' '(str) bob' '(dbl) 3' '(str) alice' '(dbl) 5' '(arr) end')" \
    zquery z -inf "" 1 10

expect "zquery with limit caps members" \
"$(printf '%s\n' '(arr) len=2' '(str) carol' '(dbl) 2' '(arr) end')" \
    zquery z -inf "" 0 1

expect "zquery seeks by score" \
"$(printf '%s\n' '(arr) len=2' '(str) alice' '(dbl) 5' '(arr) end')" \
    zquery z 4 "" 0 10

# zrem removes count reflects existence
expect "zrem present member -> 1"       "(int) 1"      zrem z bob
expect "zrem absent member -> 0"        "(int) 0"      zrem z bob
expect "zscore after zrem -> nil"       "(nil)"        zscore z bob

# type errors string command on a zset key and the reverse
expect "make a string key"              "(nil)"                 set strkey hello
expect "get on a zset key -> type err"  "(err) 3 not a string"  get z
expect "zscore on a string key"         "(err) 3 not a zset"    zscore strkey anything

# bad argument parsing
expect "zadd non numeric score"         "(err) 4 expect a number"  zadd z notanumber x
expect "zquery non numeric offset"      "(err) 4 expect an int"    zquery z 0 "" bad 10

# error paths
expect "unknown command"                "(err) 1 unknown command."  bogus x
expect "get wrong arity (too few)"      "(err) 1 unknown command."  get
expect "get wrong arity (too many)"     "(err) 1 unknown command."  get a b
expect "set wrong arity"                "(err) 1 unknown command."  set onlykey

echo "integration: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]]
