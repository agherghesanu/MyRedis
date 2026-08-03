#!/usr/bin/env bash
# idle connection timeout test for chapter 12
# starts a server with a short idle timeout then checks two things with raw sockets
# an idle connection is closed near the deadline and a busy one is kept alive
#
# usage test_timer.sh server_binary
# needs python3 since the stock client cannot hold a socket open and idle
set -u

SERVER="${1:-./redis_server}"
TIMEOUT_MS=800

# start the server with the shortened idle timeout
REDIS_IDLE_TIMEOUT_MS="$TIMEOUT_MS" "$SERVER" &
SRV=$!
trap 'kill "$SRV" 2>/dev/null' EXIT
sleep 0.4

python3 - "$TIMEOUT_MS" <<'PY'
import socket, time, struct, sys

timeout_s = int(sys.argv[1]) / 1000.0

def frame(parts):
    body = struct.pack("<I", len(parts))
    for p in parts:
        b = p.encode()
        body += struct.pack("<I", len(b)) + b
    return struct.pack("<I", len(body)) + body

def one(cmd):
    s = socket.create_connection(("127.0.0.1", 1234))
    s.sendall(frame(cmd))
    n = struct.unpack("<I", s.recv(4))[0]
    return s, s.recv(n)

fails = 0

# an idle connection should be reaped close to the deadline not far off it
s = socket.create_connection(("127.0.0.1", 1234))
s.settimeout(timeout_s + 5)
t0 = time.time()
try:
    data = s.recv(100)               # returns b"" when the server closes the socket
    dt = time.time() - t0
    if data == b"" and timeout_s * 0.6 <= dt <= timeout_s + 2.0:
        print(f"  ok: idle connection reaped after {dt:.2f}s")
    else:
        print(f"  FAIL: idle close data={data!r} dt={dt:.2f}s want ~{timeout_s:.2f}s")
        fails += 1
except socket.timeout:
    print("  FAIL: idle connection never closed")
    fails += 1
s.close()

# traffic well past the timeout must keep a connection alive
s = socket.create_connection(("127.0.0.1", 1234))
s.settimeout(5)
end = time.time() + timeout_s * 3     # run three timeouts worth of activity
i = 0
alive = True
while time.time() < end:
    try:
        s.sendall(frame(["set", "k", f"v{i}"]))
        n = struct.unpack("<I", s.recv(4))[0]
        s.recv(n)
    except (BrokenPipeError, ConnectionResetError, struct.error):
        alive = False
        break
    i += 1
    time.sleep(timeout_s / 3)
if alive:
    print(f"  ok: active connection survived {timeout_s*3:.2f}s of traffic")
else:
    print("  FAIL: active connection was closed while busy")
    fails += 1
s.close()

# a fresh connection after all that still works so the server is healthy
try:
    s, body = one(["get", "k"])
    if body[:1] == b"\x02":            # tag_str
        print("  ok: server still serving new connections")
    else:
        print(f"  FAIL: unexpected reply {body!r}")
        fails += 1
    s.close()
except Exception as ex:
    print(f"  FAIL: new connection error {ex}")
    fails += 1

print(f"timer: {'all ok' if fails == 0 else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)
PY
