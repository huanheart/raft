#include "clerk.h"

#include "raftServerRpcUtil.h"

#include "util.h"

#include <string>
#include <vector>

//这个函数单纯获取key值，这一个请求也相当于是一个日志了
std::string Clerk::Get(std::string key)
{
    m_requestId++;
    auto requestId = m_requestId;
    int server = m_recentLeaderId;
    raftKVRpcProctoc::GetArgs args;
    args.set_key(key);
    args.set_clientid(m_clientId);
    args.set_requestid(requestId);

    while(true){
        raftKVRpcProctoc::GetReply reply;
        bool ok = m_servers[server]->Get(&args, &reply);
        if(!ok || reply.err()==ErrWrongLeader  ){
            server = (server + 1) % m_servers.size();
            continue;
        }
        if (reply.err() == ErrNoKey) {
            m_recentLeaderId=server;
            return "";
        }
        if(reply.err()==OK){
            m_recentLeaderId=server;
            return reply.value();
        }
    }
    return "";
}

//这个其实就是客户端和领导者通信的过程了
void Clerk::PutAppend(std::string key, std::string value, std::string op)
{
    //当前的请求+1
    m_requestId++;
    auto requestId = m_requestId;
    auto server = m_recentLeaderId;

    while(true){
        // 这些参数是在PutAppendArgs 这个定义中
        raftKVRpcProctoc::PutAppendArgs args;
        args.set_key(key);
        args.set_value(value);
        args.set_op(op);
        args.set_clientid(m_clientId);
        args.set_requestid(requestId);
        raftKVRpcProctoc::PutAppendReply reply;
        bool ok = m_servers[server]->PutAppend(&args, &reply); //调用这个方法.//可能是修改（如插入数据或者追加、更新数据数据的方式
        if (!ok || reply.err() == ErrWrongLeader) {
            DPrintf("【Clerk::PutAppend】原以为的leader：{%d}请求失败，向新leader{%d}重试  ，操作：{%s}", server, server + 1,
                  op.c_str());
            if (!ok) {
                DPrintf("重试原因 ，rpc失敗 ，");
            }
            if (reply.err() == ErrWrongLeader) {
                DPrintf("重試原因：非leader");
            }
            server = (server + 1) % m_servers.size();  // try the next server
            continue;
        }
        if(reply.err()==OK){
            m_recentLeaderId=server;  //说明找到了领导者，且发送信息成功了
            return ;
        }
    }

}


void Clerk::Put(std::string key,std::string value)
{
    PutAppend(key, value, "Put");
}

void Clerk::Append(std::string key,std::string value)
{
    PutAppend(key, value, "Append");
}

void Clerk::Init(std::string configFileName)
{
    //获取所有的ip和端口，并进行连接，通过MprpcConfig类来将文件中写入的空格给进行删除，以及将其缓存到对应的哈希表里面
    MprpcConfig config;
    config.LoadConfigFile(configFileName.c_str());
    std::vector<std::pair<std::string, short>> ipPortVt;
    for (int i = 0; i < INT_MAX - 1; ++i) {
        std::string node = "node" + std::to_string(i);

        std::string nodeIp = config.Load(node + "ip");
        std::string nodePortStr = config.Load(node + "port");
         //由于这里默认弄了INT_MAX个，但是实际上并不一定文件中有这么多个集群服务器，固然当读取到文件没有这个的时候，就说明已经到头了
        if (nodeIp.empty()) {   
            break;
        }
        ipPortVt.emplace_back(nodeIp, atoi(nodePortStr.c_str()));  //沒有atos方法，可以考慮自己实现
    }

    //开始连接
    for(const auto&item : ipPortVt){
        std::string ip=item.first;
        short port =item.second;
        auto * rpc=new raftServerRpcUtil(ip, port); 
        //将其连接缓存起来
        m_servers.push_back(std::shared_ptr<raftServerRpcUtil>(rpc) );
    }
}


Clerk::Clerk() : m_clientId(Uuid()), m_requestId(0), m_recentLeaderId(0) {}