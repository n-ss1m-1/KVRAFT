#!/bin/bash
cd ~/Code/my_kvraft

LOG0="./data/node0/raft.log"
LOG1="./data/node1/raft.log"
LOG2="./data/node2/raft.log"

echo "===== Node0 raft.log ====="
cat $LOG0
echo "===== Node1 raft.log ====="
cat $LOG1
echo "===== Node2 raft.log ====="
cat $LOG2

echo -e "\n==== Diff check node0 vs node1 ===="
diff $LOG0 $LOG1
if [ $? -eq 0 ];then
    echo "✅ node0 == node1"
else
    echo "❌ node0 != node1 日志不一致！"
fi

echo -e "\n==== Diff check node0 vs node2 ===="
diff $LOG0 $LOG2
if [ $? -eq 0 ];then
    echo "✅ node0 == node2"
else
    echo "❌ node0 != node2 日志不一致！"
fi
