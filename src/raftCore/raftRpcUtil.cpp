#include "raftRpcUtil.h"

#include <mprpcchannel.h>
#include <mprpccontroller.h>

//MprpcChannel以及MprpcController,是我们通过继承谷歌这个有关类而来的，固然这也是为什么可以进行调用谷歌自己生成的
//方法的时候可以传入我们自定义的参数，它里面会进行一个多态
//而stub是我们自己选择开启的，它帮助我们做一个代理，然后我们可以进行调用之前在proto文件中自定义的方法(具体实现是他们帮我们生成的)

bool RaftRpcUtil::AppendEntries(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response) 
{
    MprpcController controller;
    stub_->AppendEntries(&controller, args, response, nullptr);
    return !controller.Failed();
}

bool RaftRpcUtil::InstallSnapshot(raftRpcProctoc::InstallSnapshotRequest *args,
                                  raftRpcProctoc::InstallSnapshotResponse *response) 
{
    MprpcController controller;
    stub_->InstallSnapshot(&controller, args, response, nullptr);
    return !controller.Failed();
}

bool RaftRpcUtil::RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response) 
{
    MprpcController controller;
    stub_->RequestVote(&controller, args, response, nullptr);
    return !controller.Failed();
}


//在进行连接的时候，即很多东西会进行构造函数初始化一些东西，但是我们这个服务肯定是要先开启了当前服务器，其他服务器才能进行连接的
//固然我们可以先使其所有服务器全部开启，然后再连接，也可以给一个时间间隔在连接的时候，以至于开启服务器会优先完成
RaftRpcUtil::RaftRpcUtil(std::string ip, short port) 
{
  //*********************************************  */
  //发送rpc设置
    stub_ = new raftRpcProctoc::raftRpc_Stub(new MprpcChannel(ip, port, true));
}

RaftRpcUtil::~RaftRpcUtil() 
{
    delete stub_; 
}








