# KVstorageBaseRaft-cpp

基于Raft的Kv存储数据库

## 使用方式

### 库准备
* muduo 
* boost(由于muduo依赖于boost库，固然也需要安装)
* protoc 

* protoc，本地版本为3.12.4，ubuntu22使用sudo apt-get install protobuf-compiler libprotobuf-dev安装默认就是这个版本
* boost，sudo apt-get install libboost-dev libboost-test-dev libboost-all-dev
* muduo,https://blog.csdn.net/QIANGWEIYUAN/article/details/89023980

### example说明
包含3个测试案例
* fiberExample:测试协程库
* rpcExample ：测试rpc设计，即判断该rpc是否能正常通信
* raftCoreExample： 测试KvServer，开启3个进程模拟3个raft结点间的通信(使用raft集群,基于rpcExample)
 
### 编译启动
#### 使用rpc(rpcExample)
```cpp
mkdir cmake-build-debug
cd cmake-build-debug
cmake ..
make
```
之后在目录bin就有对应的可执行文件生成
* consumer
* provider 运行即可，注意先运行provider，再运行consumer，原因很简单：需要先提供rpc服务，才能去调用。

#### 使用raft集群(raftCoreExample) && 使用kv
```cpp
mkdir cmake-build-debug
cd cmake-build-debug
cmake..
make
```
开始启动
```cpp
#表示开启3个raft集群结点,配置文件读取test.conf
raftCoreRun -n 3 -f test.conf
```

使用kv
```cpp
在启动raft集群之后启动callerMain即可
```