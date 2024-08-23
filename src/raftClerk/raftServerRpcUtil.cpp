#include "raftServerRpcUtil.h"

//由于在raftKVRpcProctoc中定义了这些方法以及参数以及命名空间的名字，那么就可以直接使用这些方法了

raftServerRpcUtil::raftServerRpcUtil(std::string ip,short port)
{
    //接收rpc设置以及发送rpc设置
    stub=new raftKVRpcProctoc::kvServerRpc_Stub(new MprpcChannel(ip,port,false) ); //这里由于是false，固然还没有创建连接，使用延迟连接
    //这里用于告诉clerk需要连接的服务器ip，端口号这些
}

raftServerRpcUtil::~raftServerRpcUtil() { delete stub; }

//可能是用于获取内容的方法
bool raftServerRpcUtil::Get(raftKVRpcProctoc::GetArgs * GetArgs ,raftKVRpcProctoc::GetReply * reply )
{
    MprpcController controller;
    stub->Get(&controller,GetArgs,reply,nullptr);
    return !controller.Failed();
}

//可能是修改（如插入数据或者追加、更新数据数据的方式
bool raftServerRpcUtil::PutAppend(raftKVRpcProctoc::PutAppendArgs * args,raftKVRpcProctoc::PutAppendReply * reply )
{
    MprpcController controller;
    stub->PutAppend(&controller, args, reply, nullptr);
    if (controller.Failed()) {
        std::cout << controller.ErrorText() << endl;
    }
     return !controller.Failed();
}
