#!/bin/bash

mkdir -p ext
file_name=`curl -s http://mirror.bit.edu.cn/apache/zookeeper/stable/|\grep -o -P "zookeeper-.*?\.tar\.gz"`
file_name=($file_name)
file_name=${file_name[0]}
curl "http://mirror.bit.edu.cn/apache/zookeeper/stable/$file_name" -o ext/$file_name
cd ext
tar -xf $file_name
cd -
zk_dir=${file_name:0:-7}
echo ZOOKEEPER_DIR="ext/$zk_dir" > zookeeper.mk
cd ext/$zk_dir/src/c
./configure
make
