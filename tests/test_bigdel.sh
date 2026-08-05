#!/usr/bin/env bash
# chapter 14 async big value deletion
# builds a sorted set past the large container threshold then deletes it
# the delete must not hang and the server must stay responsive afterward
# since the destructor runs on a worker thread not the event loop
#
# usage test_bigdel.sh server_binary
set -u

SERVER="${1:-./redis_server}"
"$SERVER" &
SRV=$!
trap 'kill "$SRV" 2>/dev/null' EXIT
sleep 0.4

python3 - <<'PY'
import socket, struct, sys, time

def frame(parts):
    body = struct.pack("<I", len(parts))
    for p in parts:
        b = p.encode()
        body += struct.pack("<I", len(b)) + b
    return struct.pack("<I", len(body)) + body

def read_reply(s):
    hdr = b""
    while len(hdr) < 4:
        hdr += s.recv(4 - len(hdr))
    n = struct.unpack("<I", hdr)[0]
    buf = b""
    while len(buf) < n:
        buf += s.recv(n - len(buf))
    return buf

s = socket.create_connection(("127.0.0.1", 1234))
s.settimeout(10)

# build a zset of 5000 members which is well past the 1000 threshold
N = 5000
for i in range(N):
    s.sendall(frame(["zadd", "big", str(i), f"m{i}"]))
    read_reply(s)

# delete it and time the call it must return promptly not after freeing 5000 nodes
t0 = time.time()
s.sendall(frame(["del", "big"]))
r = read_reply(s)
dt = time.time() - t0
ok_del = (r == b"\x03" + struct.pack("<q", 1))   # tag_int value 1
print(f"  {'ok' if ok_del else 'FAIL'}: big del returned {dt*1000:.1f}ms reply={r!r}")

# the key is unlinked immediately so keys must be empty
s.sendall(frame(["keys"]))
r = read_reply(s)
ok_keys = (r[:5] == b"\x05" + struct.pack("<I", 0))   # tag_arr len 0
print(f"  {'ok' if ok_keys else 'FAIL'}: keyspace empty after del reply={r!r}")

# the server must still answer new commands ie the loop never froze
s.sendall(frame(["set", "probe", "alive"]))
read_reply(s)
s.sendall(frame(["get", "probe"]))
r = read_reply(s)
ok_alive = (r[:1] == b"\x02" and r.endswith(b"alive"))
print(f"  {'ok' if ok_alive else 'FAIL'}: server still responsive reply={r!r}")

s.close()
fails = (not ok_del) + (not ok_keys) + (not ok_alive)
print(f"bigdel: {'all ok' if fails == 0 else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)
PY
