#include "include/mprpcchannel.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include "include/mprpccontroller.h"
#include "rpcheader.pb.h"
#include "../common/include/util.h"

/*
header_size + service_name method_name args_size + args
*/

//这个函数只要通过stub代理调用rpc方法，那么都会走到这里，
//该函数用来左数据的序列化以及网络发送的
//这里的done这个参数是一个回调函数，在关闭的时候执行这个函数
void MprpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                              google::protobuf::RpcController* controller, const google::protobuf::Message* request,
                              google::protobuf::Message* response, google::protobuf::Closure* done)
{
    if (m_clientFd == -1) {
        std::string errMsg;
        bool rt = newConnect(m_ip.c_str(), m_port, &errMsg);
        if (!rt) {
            DPrintf("[func-MprpcChannel::CallMethod]重连接ip：{%s} port{%d}失败", m_ip.c_str(), m_port);
            controller->SetFailed(errMsg);
            return;
        } else {
            DPrintf("[func-MprpcChannel::CallMethod]连接ip：{%s} port{%d}成功", m_ip.c_str(), m_port);
        }
    }

    const google::protobuf::ServiceDescriptor * sd=method->service(); //得到一个服务描述符,方便知道一些信息
    std::string service_name = sd->name();    
    std::string method_name = method->name();  

    //获取参数的序列化字符串的长度
    uint32_t args_size{};
    std::string args_str;
    //将其序列化成字符串，方便获取长度
    if (request->SerializeToString(&args_str)) {
        args_size = args_str.size();
    } else {
        controller->SetFailed("serialize request error!");
        return;
    }
    RPC::RpcHeader rpcHeader;
    rpcHeader.set_service_name(service_name);
    rpcHeader.set_method_name(method_name);
    rpcHeader.set_args_size(args_size);
   
    //获取头部大小信息
    std::string rpc_header_str;
    if (!rpcHeader.SerializeToString(&rpc_header_str)) {
        controller->SetFailed("serialize rpc header error!");
        return;
    }

      //这里主要是将一些信息转换成数据流，以方便后面发送数据流
    std::string send_rpc_str;  // 用来存储最终发送的数据
    {
        // 创建一个StringOutputStream用于写入send_rpc_str
        google::protobuf::io::StringOutputStream string_output(&send_rpc_str);
        google::protobuf::io::CodedOutputStream coded_output(&string_output);

        // 先写入header的长度（变长编码）
        coded_output.WriteVarint32(static_cast<uint32_t>(rpc_header_str.size()));

       // 不需要手动写入header_size，因为上面的WriteVarint32已经包含了header的长度信息
        // 然后写入rpc_header本身
        coded_output.WriteString(rpc_header_str);
    }

    //最后，将请求参数附加到send_rpc_str后面
    send_rpc_str+= args_str;
    //现在开始发送rpc请求,传的其实就是流，这个string字符串就是流了，由于因为有一些rpc的头部信息，固然要使用该code这个东西
    while(-1==send(m_clientFd,send_rpc_str.c_str(),send_rpc_str.size(),0) ){
        char errtxt[512] = {0};
        sprintf(errtxt, "send error! errno:%d", errno);
        std::cout << "尝试重新连接，对方ip：" << m_ip << " 对方端口" << m_port << std::endl;
        close(m_clientFd);
        m_clientFd = -1;
        std::string errMsg;
        bool rt = newConnect(m_ip.c_str(), m_port, &errMsg);
        if (!rt) {
            controller->SetFailed(errMsg);
            return;
        }
    }
    //这里是阻塞处理（所以我们一定能接收到信息,且只用一次（跟缓冲区大小设置有关）)
    //接收rpc请求的响应值
    //为什么调用一次recv就行了，webserver中调用多次？具体看文档：
    char recv_buf[1024] = {0};
    int recv_size = 0;
    if (-1 == (recv_size = recv(m_clientFd, recv_buf, 1024, 0))) {
        close(m_clientFd);
        m_clientFd = -1;
        char errtxt[512] = {0};
        sprintf(errtxt, "recv error! errno:%d", errno);
        controller->SetFailed(errtxt);
    return;
    }
    //反序列化rpc调用的响应数据
    if (!response->ParseFromArray(recv_buf, recv_size)) {
        char errtxt[1050] = {0};
        sprintf(errtxt, "parse error! response_str:%s", recv_buf);
        controller->SetFailed(errtxt);
        return;
    }
}


bool MprpcChannel::newConnect(const char* ip, uint16_t port, string* errMsg)
{
    //下面是客户端创建一个socket连接
    // 创建套接字
    int clientfd=socket(AF_INET,SOCK_STREAM,0); //表示使用ipv4，并且是流类型的协议，使用tcp
    if(clientfd==-1){
        char errtxt[512]={0};
        sprintf(errtxt,"create socket fail! errno: %d ",errno);
        m_clientFd=-1;
        *errMsg =errtxt;
        return false;
    }
    //绑定套接字
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);
    
    //连接rpc服务结点,连接服务端
    if(-1==connect(clientfd,(struct sockaddr*)&server_addr,sizeof(server_addr) ) ){
        close(clientfd);
        char errtxt[512] = {0};
        sprintf(errtxt, "connect fail! errno:%d", errno);
        m_clientFd = -1;
        *errMsg = errtxt;
        return false;
    }
    //成功设置客户端标识
    m_clientFd=clientfd;
    return true;
}

MprpcChannel::MprpcChannel(string ip, short port, bool connectNow) : m_ip(ip), m_port(port), m_clientFd(-1)
{
    //延迟连接的表明：如果当前不是很紧急，那么我们可以不用进行以下操作来建立连接
    if(!connectNow){
        return ;
    }
    std::string errMsg;
    auto rt = newConnect(ip.c_str(), port, &errMsg);
    int tryCount =3;
    //如果当前没有连接或者需要重新连接的话，那么就最多给它重新连接3次，如果还是失败，那么就没办法了
    while(!rt && tryCount--){
        std::cout<<errMsg<<std::endl;
        rt=newConnect(ip.c_str(), port, &errMsg);
    }

}