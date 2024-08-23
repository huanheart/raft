#ifndef RAFTSERVERRPC_H
#define RAFTSERVERRPC_H

#include <iostream>
#include "kvServerRPC.pb.h"
#include "mprpcchannel.h"
#include "mprpccontroller.h"
#include "rpcprovider.h"

//维护当前节点对其他某一个结点的所有rpc通信，包括接收其他节点的rpc和发送
//通过定义raftKVRpcProctoc命名空间以及这个proto文件，他给我们生成了自定义的类以及方法
//然后这里通过对这个类进一步封装来得到一个新的类
class raftServerRpcUtil{

private:
    raftKVRpcProctoc::kvServerRpc_Stub* stub;
public:
    //用来响应其他结点的
    bool Get(raftKVRpcProctoc::GetArgs* GetArgs, raftKVRpcProctoc::GetReply* reply);
    bool PutAppend(raftKVRpcProctoc::PutAppendArgs* args, raftKVRpcProctoc::PutAppendReply* reply);

    raftServerRpcUtil(std::string ip,short port);
    ~raftServerRpcUtil();
};

#endif