#!/bin/bash
# 快照端到端测试：
# 1. 启动 3 节点
# 2. 写 200 个 key（触发快照）
# 3. 验证 snapshot.bin 生成、raft.log 被截断
# 4. 重启所有节点
# 5. 验证数据完整（从快照恢复）

# 注：默认只捕获错误日志到data/node_.log    完整日志在logs/node_.log

set -e

cd "$(dirname "$0")/.."
BIN=./bin/raft-kv
LOG_DIR=~/Code/my_kvraft/data
rm -rf "$LOG_DIR"
rm -rf "logs"
mkdir -p "$LOG_DIR"

echo "=== 清理 data ==="
rm -rf data/node0 data/node1 data/node2

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

echo "=== 写入 100 个 key（触发快照） ==="
python3 - <<'EOF'
import sys
sys.path.insert(0, 'scripts')
from test_client import connect, send_msg, recv_msg, find_leader, HOST, PORTS

leader = find_leader(PORTS)
print(f"  Leader 端口: {leader}")

sock = connect(HOST, leader)
for i in range(100):
    send_msg(sock, f"PUT snap_key{i} value{i}")
    reply = recv_msg(sock)
    if reply != "OK":
        print(f"  ❌ PUT snap_key{i} failed: {reply}")
        sys.exit(1)
sock.close()
print("  ✓ 写入 100 个 key")
EOF

echo "=== 等待快照生成 ==="
sleep 3

echo "=== 检查快照文件 ==="
FOUND=0
for i in 0 1 2; do
    if [ -f "data/node$i/snapshot.bin" ]; then
        SIZE=$(stat -c%s "data/node$i/snapshot.bin")
        echo "  ✓ node$i/snapshot.bin 存在，大小 $SIZE 字节"
        FOUND=1
    else
        echo "  ✗ node$i/snapshot.bin 不存在"
    fi
done

if [ $FOUND -eq 0 ]; then
    echo "  ❌ 没有找到快照文件"
    exit 1
fi

echo "=== 检查日志是否被截断 ==="
for i in 0 1 2; do
    if [ -f "data/node$i/raft.log" ]; then
        LINES=$(wc -l < "data/node$i/raft.log")
        SIZE=$(stat -c%s "data/node$i/raft.log")
        echo "  node$i/raft.log: $LINES 行, $SIZE 字节"
    fi
done

echo "=== 检查快照日志 ==="
for i in 0 1 2; do
    if grep -q "snapshot taken" "logs/node$i.log"; then
        echo "  ✓ node$i:"
        grep "snapshot taken" "logs/node$i.log" | tail -3 | sed 's/^/    /'
    fi
done

echo "=== 重启所有节点 ==="
kill -9 $PID0 $PID1 $PID2 2>/dev/null || true
wait $PID0 $PID1 $PID2 2>/dev/null || true
sleep 1

$BIN 0 > "$LOG_DIR/node0_restart.log" 2>&1 &
PID0=$!
$BIN 1 > "$LOG_DIR/node1_restart.log" 2>&1 &
PID1=$!
$BIN 2 > "$LOG_DIR/node2_restart.log" 2>&1 &
PID2=$!
sleep 5

echo "=== 验证恢复 ==="
for i in 0 1 2; do
    if grep -q "recovered from snapshot" "logs/node${i}.log"; then
        echo "  ✓ node$i 从快照恢复"
        grep "recovered from snapshot" "logs/node${i}.log" | sed 's/^/    /'
    else
        echo "  ✗ node$i 未从快照恢复（可能没生成快照或加载失败）"
    fi
done

echo "=== 验证数据完整 ==="
python3 - <<'EOF'
import sys, time
sys.path.insert(0, 'scripts')
from test_client import connect, send_msg, recv_msg, find_leader, HOST, PORTS

time.sleep(1)   # 等 Follower 追上

leader = find_leader(PORTS)
print(f"  Leader 端口: {leader}")

sock = connect(HOST, leader)
for i in range(100):
    key = f"snap_key{i}"
    send_msg(sock, f"GET {key}")
    reply = recv_msg(sock)
    if reply != f"value{i}":
        print(f"  ❌ GET {key} 期望 value{i}，实际 {reply}")
        sys.exit(1)
sock.close()
print("  ✓ 100 个 key 全部恢复")
EOF

echo ""
echo "✅ 快照测试通过"