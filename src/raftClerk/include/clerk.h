#ifndef SKIP_LIST_ON_RAFT_CLERK_H
#define SKIP_LIST_ON_RAFT_CLERK_H
#include <arpa/inet.h>
#include <netinet/in.h>
#include <raftServerRpcUtil.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <vector>
#include "kvServerRPC.pb.h"
#include "mprpcconfig.h"


class Clerk{
private:
    std::vector<std::shared_ptr<raftServerRpcUtil> > m_servers; //保存所有raft结点的fd，一开始全部初始化为-1.表示没有链接上
    std::string m_clientId;
    int m_requestId;
    int m_recentLeaderId; //用于记录上一次请求的领导是谁，因为领导会随着时间变化，固然不一定现在还是领导

    //返回随机的，唯一的客户端标识给到客户端，这里是1000分之1的概率，可以使用其他库来做到
    std::string Uuid(){
        return std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand());
    }
    void PutAppend(std::string key, std::string value, std::string op);

public:
    void Init(std::string configFileName);

    void Put(std::string key,std::string value);
    void Append(std::string key,std::string value);

    std::string Get(std::string key);

public:
    Clerk();

};

#endif 

