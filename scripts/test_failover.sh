#!/bin/bash
# 故障转移测试：
# 1. 启动 3 节点
# 2. 写入数据
# 3. kill Leader
# 4. 验证新 Leader 选出
# 5. 在新 Leader 写入数据
# 6. 重启原 Leader
# 7. 验证数据一致

set -e

cd "$(dirname "$0")/.."
BIN=./bin/raft-kv
LOG_DIR=~/Code/my_kvraft/data
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

echo "=== 清理 data ==="
rm -rf data/node0 data/node1 data/node2
mkdir -p data/node0 data/node1 data/node2

echo "=== 启动 3 个节点 ==="
$BIN 0 > "$LOG_DIR/node0.log" 2>&1 &
PID0=$!
$BIN 1 > "$LOG_DIR/node1.log" 2>&1 &
PID1=$!
$BIN 2 > "$LOG_DIR/node2.log" 2>&1 &
PID2=$!

cleanup() {
    kill $PID0 $PID1 $PID2 2>/dev/null || true
    wait $PID0 $PID1 $PID2 2>/dev/null || true
}
trap cleanup EXIT

echo "  等待选举（4 秒）"
sleep 4

echo "=== 写入初始数据 ==="
python3 - <<'EOF'
import sys
sys.path.insert(0, 'scripts')
from test_client import connect, send_msg, recv_msg, find_leader, HOST, PORTS

leader = find_leader(PORTS)
if not leader:
    print("FAIL: 找不到 Leader")
    sys.exit(1)
print(f"  Leader 端口: {leader}")

sock = connect(HOST, leader)
for i in range(5):
    send_msg(sock, f"PUT init{i} v{i}")
    assert recv_msg(sock) == "OK"
sock.close()
print("  ✓ 写入 5 个 key")
EOF

echo "=== 找到并 kill Leader ==="
LEADER_PID=""
LEADER_PORT=""
for pid_port in "$PID0:8001" "$PID1:8002" "$PID2:8003"; do
    pid="${pid_port%%:*}"
    port="${pid_port##*:}"
    if python3 -c "
import sys; sys.path.insert(0, 'scripts')
from test_client import connect, send_msg, recv_msg, find_leader, HOST, PORTS
l = find_leader(PORTS)
sys.exit(0 if l == $port else 1)
"; then
        LEADER_PID=$pid
        LEADER_PORT=$port
        break
    fi
done

if [ -z "$LEADER_PID" ]; then
    echo "FAIL: 找不到 Leader 的 PID"
    exit 1
fi
echo "  Leader PID=$LEADER_PID port=$LEADER_PORT"
kill -9 "$LEADER_PID"
echo "  ✓ 已 kill"

echo "=== 等待重新选举（5 秒） ==="
sleep 5

echo "=== 验证新 Leader 选出 ==="
python3 - <<'EOF'
import sys, time
sys.path.insert(0, 'scripts')
from test_client import connect, send_msg, recv_msg, find_leader, HOST, PORTS

for attempt in range(10):
    leader = find_leader(PORTS)
    if leader:
        print(f"  ✓ 新 Leader 端口: {leader}")
        break
    time.sleep(1)
else:
    print("FAIL: 5 秒内没选出新 Leader")
    sys.exit(1)

# 验证数据还在
sock = connect(HOST, leader)
for i in range(5):
    send_msg(sock, f"GET init{i}")
    reply = recv_msg(sock)
    assert reply == f"v{i}", f"init{i} 期望 v{i}，实际 {reply}"
sock.close()
print("  ✓ 原数据仍可读")

# 写入新数据
sock = connect(HOST, leader)
send_msg(sock, "PUT newdata nv")
assert recv_msg(sock) == "OK"
sock.close()
print("  ✓ 新数据写入成功")
EOF

echo "=== 重启原 Leader ==="
if [ "$LEADER_PORT" = "8001" ]; then
    $BIN 0 > "$LOG_DIR/node0_restart.log" 2>&1 &
    PID0=$!
elif [ "$LEADER_PORT" = "8002" ]; then
    $BIN 1 > "$LOG_DIR/node1_restart.log" 2>&1 &
    PID1=$!
else
    $BIN 2 > "$LOG_DIR/node2_restart.log" 2>&1 &
    PID2=$!
fi
sleep 3

echo "=== 验证所有节点数据一致 ==="
python3 - <<'EOF'
import sys, time
sys.path.insert(0, 'scripts')
from test_client import connect, send_msg, recv_msg, HOST, PORTS

time.sleep(3)   # 等日志追上

for port in PORTS:
    try:
        sock = connect(HOST, port)
        send_msg(sock, "GET newdata")
        reply = recv_msg(sock)
        sock.close()
        if reply == "nv":
            print(f"  ✓ 端口 {port}: newdata = nv")
        else:
            print(f"  ❌ 端口 {port}: newdata = {reply}（期望 nv）")
            sys.exit(1)
    except Exception as e:
        print(f"  ❌ 端口 {port} 连接失败: {e}")
        sys.exit(1)

print("  ✓ 所有节点一致")
EOF

echo ""
echo "✅ 故障转移测试通过"