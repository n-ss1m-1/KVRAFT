#!/bin/bash
# 持久化测试：
# 1. 启动 3 节点
# 2. 写入数据
# 3. 全部 kill
# 4. 重启
# 5. 验证数据还在

set -e

cd "$(dirname "$0")/.."
BIN=./bin/raft-kv
LOG_DIR=/tmp/raft_restart_logs
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

echo "=== 清理 data ==="
rm -rf data/node0 data/node1 data/node2
mkdir -p data/node0 data/node1 data/node2

echo "=== 第一次启动 ==="
$BIN 0 > "$LOG_DIR/run1_node0.log" 2>&1 &
PID0=$!
$BIN 1 > "$LOG_DIR/run1_node1.log" 2>&1 &
PID1=$!
$BIN 2 > "$LOG_DIR/run1_node2.log" 2>&1 &
PID2=$!
sleep 4

echo "=== 写入数据 ==="
python3 - <<'EOF'
import sys
sys.path.insert(0, 'scripts')
from test_client import connect, send_msg, recv_msg, find_leader, HOST, PORTS

leader = find_leader(PORTS)
print(f"  Leader 端口: {leader}")
sock = connect(HOST, leader)
for i in range(20):
    send_msg(sock, f"PUT persistent{i} value{i}")
    assert recv_msg(sock) == "OK"
sock.close()
print("  ✓ 写入 20 个 key")
EOF

echo "=== 全部 kill ==="
kill -9 $PID0 $PID1 $PID2
wait $PID0 $PID1 $PID2 2>/dev/null || true
sleep 1

echo "=== 重启 ==="
$BIN 0 > "$LOG_DIR/run2_node0.log" 2>&1 &
PID0=$!
$BIN 1 > "$LOG_DIR/run2_node1.log" 2>&1 &
PID1=$!
$BIN 2 > "$LOG_DIR/run2_node2.log" 2>&1 &
PID2=$!
sleep 4

echo "=== 验证数据 ==="
python3 - <<'EOF'
import sys, time
sys.path.insert(0, 'scripts')
from test_client import connect, send_msg, recv_msg, find_leader, HOST, PORTS

leader = find_leader(PORTS)
print(f"  Leader 端口: {leader}")

time.sleep(1)   # 等 Follower 追上

for port in PORTS:
    sock = connect(HOST, port)
    for i in range(20):
        send_msg(sock, f"GET persistent{i}")
        reply = recv_msg(sock)
        assert reply == f"value{i}", f"端口 {port} persistent{i} 期望 value{i}，实际 {reply}"
    sock.close()
    print(f"  ✓ 端口 {port}: 20 个 key 全部恢复")

print("  ✓ 持久化测试通过")
EOF

echo ""
echo "=== 清理 ==="
kill -9 $PID0 $PID1 $PID2 2>/dev/null || true
wait 2>/dev/null || true
echo "✅ 持久化测试通过"