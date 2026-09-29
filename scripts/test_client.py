#!/usr/bin/env python3
"""
Raft KV 基础功能测试：
1. 找 Leader
2. PUT / GET / DEL
3. 覆盖写
4. 批量写
5. 验证所有节点数据一致
"""
import socket
import struct
import sys
import time

HOST = "127.0.0.1"
PORTS = [8001, 8002, 8003]


def connect(host, port, timeout=5):
    return socket.create_connection((host, port), timeout=timeout)


def send_msg(sock, msg):
    data = msg.encode('utf-8')
    sock.sendall(struct.pack('>i', len(data)) + data)


def recv_msg(sock):
    length_bytes = b''
    while len(length_bytes) < 4:
        chunk = sock.recv(4 - len(length_bytes))
        if not chunk:
            return None
        length_bytes += chunk
    length = struct.unpack('>i', length_bytes)[0]

    data = b''
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            return None
        data += chunk
    return data.decode('utf-8')


def find_leader(ports):
    """遍历端口，找到一个认为自己是 Leader 的节点"""
    for port in ports:
        try:
            sock = connect(HOST, port, timeout=2)
            send_msg(sock, "PUT __probe__ 1")
            reply = recv_msg(sock)
            sock.close()
            if reply and not reply.startswith("NOT_LEADER"):
                return port
        except Exception:
            pass
    return None


def test_basic():
    print("=" * 50)
    print("[基础测试] 找 Leader")
    print("=" * 50)

    leader_port = None
    for attempt in range(20):
        leader_port = find_leader(PORTS)
        if leader_port:
            break
        print(f"  第 {attempt+1} 次尝试失败，等 500ms")
        time.sleep(0.5)

    if not leader_port:
        print("❌ 找不到 Leader")
        return False

    print(f"✓ Leader 在端口 {leader_port}")

    sock = connect(HOST, leader_port)

    # --- PUT ---
    print("\n[基础测试] PUT / GET / DEL")
    send_msg(sock, "PUT hello world")
    reply = recv_msg(sock)
    assert reply == "OK", f"PUT 失败: {reply}"
    print(f"  PUT hello world → {reply}")

    send_msg(sock, "GET hello")
    reply = recv_msg(sock)
    assert reply == "world", f"GET 失败: {reply}"
    print(f"  GET hello → {reply}")

    # --- 覆盖写 ---
    send_msg(sock, "PUT hello world2")
    reply = recv_msg(sock)
    assert reply == "OK"
    send_msg(sock, "GET hello")
    reply = recv_msg(sock)
    assert reply == "world2", f"覆盖写失败: {reply}"
    print(f"  覆盖写 hello → {reply}")

    # --- DEL ---
    send_msg(sock, "DEL hello")
    reply = recv_msg(sock)
    assert reply == "OK", f"DEL 失败: {reply}"
    send_msg(sock, "GET hello")
    reply = recv_msg(sock)
    assert reply == "nil", f"DEL 后 GET 应为 nil: {reply}"
    print(f"  DEL hello → OK，再 GET → {reply}")

    # --- 批量写 ---
    print("\n[基础测试] 批量写 100 个 key")
    for i in range(100):
        send_msg(sock, f"PUT key{i:03d} value{i}")
        reply = recv_msg(sock)
        assert reply == "OK", f"PUT key{i} 失败: {reply}"

    for i in range(100):
        send_msg(sock, f"GET key{i:03d}")
        reply = recv_msg(sock)
        assert reply == f"value{i}", f"GET key{i} 期望 value{i}，实际 {reply}"
    print(f"  ✓ 100 个 key 全部写入并读回成功")

    sock.close()
    return True


def test_replication():
    """验证所有节点的状态机数据一致"""
    print("\n" + "=" * 50)
    print("[一致性测试] 验证 3 个节点数据一致")
    print("=" * 50)

    # 等 Follower 追上（下一次心跳会带着 leaderCommit）
    time.sleep(0.5)

    for port in PORTS:
        sock = connect(HOST, port)
        # 抽查几个 key
        mismatched = []
        for i in [0, 50, 99]:
            key = f"key{i:03d}"
            send_msg(sock, f"GET {key}")
            reply = recv_msg(sock)
            expected = f"value{i}"
            if reply != expected:
                mismatched.append((key, expected, reply))
        sock.close()

        if mismatched:
            print(f"  ❌ 端口 {port} 数据不一致:")
            for k, exp, act in mismatched:
                print(f"     {k}: 期望 {exp}，实际 {act}")
            return False
        else:
            print(f"  ✓ 端口 {port} 数据一致")

    return True


if __name__ == '__main__':
    try:
        if not test_basic():
            sys.exit(1)
        if not test_replication():
            sys.exit(1)
        print("\n" + "=" * 50)
        print("✅ 全部测试通过")
        print("=" * 50)
        sys.exit(0)
    except AssertionError as e:
        print(f"\n❌ 测试失败: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"\n❌ 异常: {e}")
        sys.exit(1)