# MyRedis

A single-threaded, in-memory, data-structure server written from scratch in C++, a compact
re-implementation of the core ideas behind Redis. It speaks a binary request/response protocol
over TCP, holds all state in memory, and multiplexes many concurrent clients on one event loop
without ever locking its data structures.

This is a learning-grade server, not a drop-in Redis replacement. It implements the engine that
matters, the event loop, the protocol, the data structures, expiration, and asynchronous cleanup,
and leaves out persistence, replication, clustering, and the full command surface.

---

## Features

- Nonblocking event loop over `poll`, one thread serving all connections.
- Binary framed protocol with typed, nestable replies (nil / error / string / int / double / array).
- String key/value commands: `get`, `set`, `del`, `keys`.
- Sorted sets backed by an AVL tree *and* a hashtable at once: `zadd`, `zrem`, `zscore`, `zquery`.
- Key expiration (TTL) via a min-heap: `pexpire`, `pttl`, with lazy-free of expired keys in the loop.
- Idle-connection reaping via a time-ordered linked list.
- Thread pool that offloads the destruction of very large values so the event loop never stalls.
- Concurrent benchmark harness that drives the server from many client threads and reports
  throughput and latency percentiles.
- Full test suite: unit tests for every data structure plus end-to-end socket tests.

---

## Architecture

### One thread, many clients

The server is single-threaded. All command execution happens on one event loop:

1. Build the set of sockets to watch from each connection's current interest (`want_read` / `want_write`).
2. `poll`, block until any socket is ready or the nearest timer is due.
3. Service each ready socket: read bytes, parse complete requests, execute, buffer replies, write.
4. Fire timers (idle connections, expired keys).

Sockets are nonblocking, so a slow or stalled client can never freeze the loop, a read that would
block simply returns `EAGAIN` and the loop moves on. Each connection owns `incoming` and `outgoing`
byte buffers because TCP is a stream: a request may arrive split across several wakeups, and a reply
may drain across several writes.

Why single-threaded? Because only one command runs at any instant, the shared data structures
need no locks at all. This removes an entire class of bugs and overhead. The price is that command
execution cannot use multiple cores, one loop saturates a single core, and past that point additional
clients add latency rather than throughput. This is the same trade Redis itself makes.

### Storage: the hashtable spine

The keyspace is a single hashtable mapping a key string to an `Entry`. Everything routes through it.
Notable design points:

- Separate chaining with a cached hash code per node, so a lookup rejects non-matching chain
  entries with a cheap integer comparison before doing a full key comparison.
- Progressive resizing. When the table grows past its load factor it does not rehash all
  entries at once (an O(n) latency spike that would freeze the loop). It keeps two tables and migrates
  a small bounded batch of entries on every subsequent operation, checking both tables during the
  transition. The cost of a large resize is smeared across many operations, keeping per-operation
  latency bounded.

### Data structures

| Structure          | Used for                              | Complexity                     |
| ------------------ | ------------------------------------- | ------------------------------ |
| Hashtable          | the keyspace; sorted-set member index | O(1) average lookup/insert     |
| AVL tree           | sorted-set ordering by (score, name)  | O(log n) insert/delete/seek    |
| Sorted set (ZSet)  | a member in the hashtable + AVL tree  | O(1) by name, O(log n) by rank |
| Binary min-heap    | TTL deadlines                         | O(log n) update, O(1) min      |
| Doubly-linked list | idle-connection ordering              | O(1) touch                     |

The sorted set is the centerpiece: each member is indexed two ways simultaneously, by name in a
hashtable (answer "what is alice's score?" in O(1)) and by (score, name) in an AVL tree (answer "who
ranks 5th to 10th?" in O(log n)). The AVL nodes cache subtree sizes so rank queries are logarithmic
rather than linear.

### Intrusive data structures

Every container is intrusive: the node lives *inside* the value struct rather than in a separate
wrapper, and the owning object is recovered from the node with a `container_of` pointer offset.

```c
struct Entry { HNode node; std::string key; /* value fields */ };
struct ZNode { AVLNode tree; HNode hmap; double score; size_t len; char name[0]; };
```

This means one allocation per object instead of two, no pointer indirection from a generic node to a
separate value, and container code that stays fully generic (the hashtable only ever knows about
`HNode`; comparison and hashing are injected as function pointers). The `char name[0]` flexible array
member stores a sorted-set member's name inline, so the whole member is a single allocation.

### Protocol

- Requests are length-prefixed frames: a total length, a count of arguments, then each argument as
  a length followed by its bytes. The length prefix lets the reader know exactly when a full message
  has arrived on the byte stream.
- Replies use a tag/length/value scheme. Every value carries a one-byte type tag
  (`nil`, `err`, `str`, `int`, `dbl`, `arr`) followed by a type-specific payload. Arrays declare a
  count and then contain that many tagged values, so replies nest arbitrarily and the client parses
  them recursively. This is a binary twin of Redis's RESP protocol, the same concepts, encoded in
  binary rather than text for simplicity.
- The server serializes a reply directly into the outgoing buffer with a placeholder length, then
  backfills the real length once the reply is complete, avoiding a temporary buffer and a copy.

### Timers

Two independent timers, each using the structure that fits it:

- Idle connection timeout uses a doubly-linked list. Every connection shares the same timeout, so
  moving a connection to the back of the list on each I/O keeps the list ordered by deadline for free,
  an O(1) operation, appropriate because connections are touched on every read and write.
- Key TTL uses a min-heap. Each key expires at its own arbitrary time, so the linked-list trick
  does not apply; the heap yields the earliest deadline in O(1) and reschedules any key in O(log n).
  A back-pointer stored in each heap slot lets a key find and update its own deadline without scanning.

The loop computes its `poll` timeout as the minimum of both next-deadlines, and processes both kinds of
timer after servicing sockets. Expired keys are evicted in bounded batches so that a mass expiration
cannot stall the loop.

### Thread pool

The single-threaded rule has exactly one escape hatch. Deleting a very large value (for example, a
sorted set with millions of members) means millions of individual frees; done synchronously it would
freeze the loop. So `del` and TTL expiration unlink the entry from every shared structure on the loop
thread first, while nothing else can touch it, and then hand the now-unreachable object to a worker
thread pool for the actual free. Because the object is already unreachable, the workers need no locks on
the data; only the task queue is synchronized (a mutex plus a condition variable). Small values are
freed inline, since a context switch would cost more than the free itself.

---

## Design decisions and trade-offs

| Decision                        | Gains                                                     | Costs                                                        |
| ------------------------------- | -------------------------------------------------------- | ----------------------------------------------------------- |
| Single-threaded event loop      | No data locks; simple; predictable latency               | No multi-core command execution; saturates one core         |
| In-memory storage               | Microsecond operations                                   | Dataset bounded by RAM; no durability                       |
| Intrusive nodes + `container_of`| One allocation per object; cache-friendly; generic code  | Lower-level, manual memory layout                           |
| Progressive hashtable resize    | No O(n) latency spike when the map grows                 | Two live tables; lookups probe both during migration        |
| Binary protocol                 | Trivial to parse and generate                            | Not wire-compatible with the real `redis-cli`               |
| Nonblocking sockets + buffering | One slow client cannot stall the loop                    | Partial reads and writes must be handled everywhere         |
| Async free of large values      | The loop never blocks on a huge delete                   | The only threaded code path; requires unlink-before-hand-off|
| Exact-key access only           | Predictable O(1) / O(log n) operations                   | No ad-hoc queries or joins; data is shaped around access    |
| `poll` (not `epoll`)            | Portable POSIX; simple                                   | O(n) per iteration; scales worse to very many idle sockets  |

---

## Project layout

```
src/
  server.cpp        event loop, protocol, command dispatch, timers, TTL
  client.cpp        minimal command-line client
  hashtable.{h,cpp} intrusive hashtable with progressive resizing
  avl.{h,cpp}       intrusive AVL tree with rank support
  zset.{h,cpp}      sorted set (AVL tree + hashtable)
  heap.{h,cpp}      binary min-heap for TTL deadlines
  dlist.h           intrusive doubly-linked list for idle connections
  thread_pool.{h,cpp} worker pool for asynchronous large-value destruction
tests/
  test_hashtable.cpp  test_avl.cpp  test_zset.cpp  test_heap.cpp
  test_thread_pool.cpp
  test_integration.sh  test_timer.sh  test_bigdel.sh
bench/
  bench.cpp         concurrent load generator
CMakeLists.txt
```

---

## Building

The server uses POSIX sockets and `poll`, so it targets Linux (native, WSL, a VM, or a container).

```bash
cmake -S . -B build
cmake --build build
```

This produces `redis_server`, `redis_client`, `bench`, and the test executables in `build/`.

---

## Running

Start the server (listens on port `1234`):

```bash
./build/redis_server
```

In another terminal, use the client. It sends one command and prints the typed reply:

```bash
./build/redis_client set foo hello       # (nil)
./build/redis_client get foo             # (str) hello
./build/redis_client del foo             # (int) 1
./build/redis_client zadd board 100 ada  # (int) 1
./build/redis_client zadd board 250 bo   # (int) 1
./build/redis_client zquery board -inf "" 0 10
#   (arr) len=4
#   (str) ada
#   (dbl) 100
#   (str) bo
#   (dbl) 250
#   (arr) end
./build/redis_client pexpire foo 5000    # (int) 1
./build/redis_client pttl foo            # (int) 4998
```

The idle-connection timeout defaults to 5 seconds and can be overridden (mainly for testing) with the
`REDIS_IDLE_TIMEOUT_MS` environment variable.

---

## Command reference

| Command                              | Reply                                  | Notes                                              |
| ------------------------------------ | -------------------------------------- | -------------------------------------------------- |
| `get key`                            | string, or nil if missing              | Errors if the key holds a non-string value         |
| `set key value`                      | nil                                    | Creates or overwrites a string                     |
| `del key`                            | int (1 removed, 0 absent)              | Large values are freed asynchronously              |
| `keys`                               | array of all key names                 |                                                    |
| `zadd key score member`              | int (1 added, 0 updated)               | Creates the sorted set if needed                   |
| `zrem key member`                    | int (1 removed, 0 absent)              |                                                    |
| `zscore key member`                  | double, or nil if absent               |                                                    |
| `zquery key score name offset limit` | array of `member, score, …` pairs      | Seeks to `(score, name)`, skips `offset`, takes `limit` |
| `pexpire key ttl_ms`                 | int (1 if key exists, else 0)          | Arms or reschedules expiration                     |
| `pttl key`                           | int ms left, `-1` no TTL, `-2` missing |                                                    |

Replies are tagged values: `(nil)`, `(err) code message`, `(str) …`, `(int) …`, `(dbl) …`, and
`(arr) len=N … (arr) end`.

---

## Testing

The suite is wired into CTest:

```bash
cd build && ctest --output-on-failure
```

| Test          | Type        | What it covers                                                                 |
| ------------- | ----------- | ------------------------------------------------------------------------------ |
| `hashtable`   | unit        | Insert/lookup/delete, resize under load, correctness across a live migration   |
| `avl`         | unit        | Rotations and balance invariants after every insert and delete; rank queries   |
| `zset`        | unit        | Add/update/lookup/delete, ordering, seek, and rank against a reference model   |
| `heap`        | unit        | Heap property and back-pointer integrity under randomized operations           |
| `thread_pool` | unit        | Many queued tasks each run exactly once, with arguments intact                 |
| `integration` | end-to-end  | Every command over a real socket, including type and argument error paths      |
| `timer`       | end-to-end  | Idle connections are reaped near the deadline; busy connections are kept alive |
| `bigdel`      | end-to-end  | Deleting a large sorted set returns immediately and the server stays responsive|

The data-structure unit tests verify every operation against an independent reference (a `std::set`,
`std::multiset`, or recomputed invariants), so a subtle corruption in a rotation or a rehash is caught
rather than silently returning wrong answers.

---

## Benchmark harness

`bench` is a concurrent load generator. It opens one persistent connection per thread, pipelines
requests on each connection, and matches replies to sends in FIFO order so that pipelining does not
distort the per-request latency measurements. It pre-populates the keyspace so that reads hit.

```bash
./build/bench -t 16 -n 50000 -P 16 -w mixed -k 1000
```

| Flag | Meaning                                        | Default     |
| ---- | ---------------------------------------------- | ----------- |
| `-t` | client threads (one connection each)           | 8           |
| `-n` | requests per thread                            | 100000      |
| `-k` | keyspace size (distinct keys or members)       | 1000        |
| `-P` | pipeline depth (requests in flight per socket) | 16          |
| `-w` | workload: `get` `set` `mixed` `zadd` `zquery`  | `mixed`     |
| `-h` `-p` | host and port                             | 127.0.0.1 1234 |

### What each workload exercises

- `get`, the pure read path: hashtable lookup plus reply serialization. Isolates read throughput.
- `set`, the write path: hashtable insert or in-place update.
- `mixed`, 80% reads, 20% writes, a realistic cache-style load over a shared keyspace.
- `zadd`, every thread writes to a single sorted set, exercising the AVL-tree + hashtable dual
  index and demonstrating that the single-threaded model has no lock contention even when all clients
  target the same structure.
- `zquery`, range scans: seek into the tree and walk a page of results, exercising ordered access.

Running multiple `bench` processes in parallel (each with several threads) exercises the server from
separate address spaces, useful for ruling out client-side thread contention.

### Sample results

Measured on a single machine over loopback, server and clients sharing the same cores, `-O2` build.
Absolute numbers understate a real network deployment (here the client CPU competes with the server
CPU); the shapes are the point.

Mixed 80/20, rising thread count (`-n 50000 -P 16 -k 1000`):

| Threads | Throughput (ops/s) | p50 (µs) | p99 (µs) |
| ------- | ------------------ | -------- | -------- |
| 1       | 215,000            | 50       | 196      |
| 4       | 865,000            | 38       | 219      |
| 16      | 778,000            | 270      | 711      |
| 64      | 784,000            | 1216     | 2458     |

Throughput climbs until the single event loop saturates (around four saturating clients), then
plateaus while latency grows roughly linearly with concurrency. This is the expected signature of a
single-server queue: past saturation, added clients buy latency, not throughput.

Pure reads, pipeline sweep (`-t 16 -n 50000 -k 1000 -w get`):

| Pipeline | Throughput (ops/s) | p50 (µs) |
| -------- | ------------------ | -------- |
| 1        | 83,000             | 165      |
| 8        | 490,000            | 230      |
| 64       | 1,172,000          | 696      |

With one request in flight per connection, throughput is bounded by round-trip latency, the loop wakes
once per request. Deeper pipelining lets the loop drain many requests per wakeup, amortizing the
per-wakeup syscall cost and multiplying throughput by roughly 14×. Pipelining, not raw core speed,
is the dominant throughput lever for this design.

Single-set contention (`-t 16 -n 30000 -P 16 -w zadd -k 5000`): ~665,000 ops/s with all sixteen threads
writing the same sorted set, no slowdown from contention, because only one command touches the
structure at a time by construction.

---

## Not implemented

By design, this project focuses on the engine and omits: persistence (RDB/AOF), replication and
clustering, the full Redis command set and data types (lists, sets, hashes, streams, pub/sub),
authentication, `epoll`/`io_uring` event backends, and RESP wire compatibility. The `zquery` reply and
the request parser demonstrate the shape these would take.
```
