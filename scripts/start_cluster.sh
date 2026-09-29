#!/bin/bash
cd ~/Code/my_kvraft

# 删除持久化目录，清空旧raft日志
rm -rf ./data
mkdir -p data/node0 data/node1 data/node2

pwd
# 后台启动三个节点，输出重定向到data
./bin/raft-kv 0 > data/node0.log 2>&1 &
./bin/raft-kv 1 > data/node1.log 2>&1 &
./bin/raft-kv 2 > data/node2.log 2>&1 &

echo "等待4秒，集群选主完成..."
sleep 4
echo "3节点集群启动完成！"



# 杀死三个节点： pkill -f raft-kv
