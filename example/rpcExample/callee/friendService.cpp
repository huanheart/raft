//
// Created by swx on 23-12-21.
//
#include <mprpcchannel.h>
#include <iostream>
#include <string>
#include "rpcExample/friend.pb.h"

#include <vector>
#include "rpcprovider.h"

class FriendService : public fixbug::FiendServiceRpc {
 public:
  std::vector<std::string> GetFriendsList(uint32_t userid) {
    std::cout << "local do GetFriendsList service! userid:" << userid << std::endl;
    std::vector<std::string> vec;
    vec.push_back("gao yang");
    vec.push_back("liu hong");
    vec.push_back("wang shuo");
    return vec;
  }

  // 重写基类方法
  void GetFriendsList(::google::protobuf::RpcController *controller, const ::fixbug::GetFriendsListRequest *request,
                      ::fixbug::GetFriendsListResponse *response, ::google::protobuf::Closure *done) {
    uint32_t userid = request->userid();
    std::vector<std::string> friendsList = GetFriendsList(userid);
    response->mutable_result()->set_errcode(0);      //mutable_result返回对应的类的指针类型，他会根据成员名，自己增加errcode以及set_errcode方法
    response->mutable_result()->set_errmsg("");
    for (std::string &name : friendsList) {
      std::string *p = response->add_friends();
      *p = name;
    }
    //done->Run函数通知 gRPC 框架 RPC 调用完成,此时就可以触发rpcprovider中的回调了，此时将其数据转化成protobuf数据并通过muduo维护的send方法响应回去
    done->Run();
  }
};

int main(int argc, char **argv) {
    //服务端监听对应的ip，可以发现和客户端需要连接的ip地址不同，但是其二者都属于本机的环回地址，固然依旧可以连接上,当然也可以统一写成127.0.0.1
  std::string ip = "127.0.0.1";
  short port = 7788;
  auto stub = new fixbug::FiendServiceRpc_Stub(new MprpcChannel(ip, port, false));
  // provider是一个rpc网络服务对象。把UserService对象发布到rpc节点上
  RpcProvider provider;
  //通过这个NotifyService注册对应的RPC服务
  provider.NotifyService(new FriendService());

  // 启动一个rpc服务发布节点   Run以后，进程通过muduo的事件循环进入阻塞状态，等待远程的rpc调用请求
  provider.Run(1, 7788);
  return 0;
}
