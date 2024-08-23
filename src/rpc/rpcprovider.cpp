#include "include/rpcprovider.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <string>
#include "rpcheader.pb.h"
#include "../common/include/util.h"

//用于发布服务的接口(将其注册进到内存当中，而不是利用注册中心，后续可能会通过这个来知道哪些接口，哪些服务能被使用)
void RpcProvider::NotifyService(google::protobuf::Service *service)
{
    ServiceInfo service_info; //用于接收服务的一些信息的类

    //获取服务对象的描述信息
    const google::protobuf::ServiceDescriptor * pserviceDesc =service->GetDescriptor();

    //获取服务的名字
    std::string service_name=pserviceDesc->name();
    //获取服务对象service的方法的数量,即这个服务有多少个接口可以给我们调用?
    int method_cnt=pserviceDesc->method_count();

    std::cout << "service_name:" << service_name << std::endl;
    for(int i=0;i<method_cnt;++i){
        //获取每一个具体的接口信息，然后存到哈希表中
        const google::protobuf::MethodDescriptor * pmethodDesc = pserviceDesc->method(i);
        std::string method_name=pmethodDesc->name();
        service_info.m_methodMap.insert( {method_name,pmethodDesc} );
    }
    service_info.m_service=service;
    m_serviceMap.insert( {service_name,service_info} );
}

//当一个服务被发布之后，还需要启动别人才能使用
void RpcProvider::Run(int nodeIndex,short port)
{
    char * ipC;
    char hname[128];
    struct hostent* hent;
    gethostname(hname,sizeof(hname) ); //获取主机名称
    hent=gethostbyname(hname); //根据这个主机名称获取ip地址
    for(int i=0;hent->h_addr_list[i];i++){
        ipC = inet_ntoa(*(struct in_addr *)(hent->h_addr_list[i]) );  // IP地址
    }

    std::string ip=std::string(ipC);

    //写入文件 "test.conf"
    std::string node="node"+std::to_string(nodeIndex);
    std::ofstream outfile;
    outfile.open("test.conf",std::ios::app); //打开文件并追加写入
    if(!outfile.is_open()){
        std::cout<<"file open failed 54"<<std::endl;
        exit(EXIT_FAILURE); //非正常退出
    }
    outfile << node + "ip=" + ip << std::endl;
    outfile << node + "port=" + std::to_string(port) << std::endl;
    outfile.close();

    //InetAddress保存了ip地址以及端口
    muduo::net::InetAddress address(ip,port);

    // 创建TcpServer对象
    m_muduo_server = std::make_shared<muduo::net::TcpServer>(&m_eventLoop, address, "RpcProvider");
 // 绑定连接回调和消息读写回调方法  分离了网络代码和业务代码
  /*
  bind的作用：
  如果不使用std::bind将回调函数和TcpConnection对象绑定起来
  那么在回调函数中就无法直接访问和修改TcpConnection对象的状态。
  因为回调函数是作为一个独立的函数被调用的，它没有当前对象的上下文信息（即this指针）,
  也就无法直接访问当前对象的状态。
  如果要在回调函数中访问和修改TcpConnection对象的状态，需要通过参数的形式将当前对象的指针传递进去，
  并且保证回调函数在当前对象的上下文环境中被调用。这种方式比较复杂，容易出错，也不便于代码的编写和维护。
  因此，使用std::bind将回调函数和TcpConnection对象绑定起来，可以更加方便、直观地访问和修改对象的状态，
  同时也可以避免一些常见的错误。
  */
    //设置连接的时候的回调
    m_muduo_server->setConnectionCallback(std::bind(&RpcProvider::OnConnection, this, std::placeholders::_1));
    //设置有读写事件发生的时候的回调
    m_muduo_server->setMessageCallback(
    std::bind(&RpcProvider::OnMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    //设置该muduo库服务有多少个线程
    m_muduo_server->setThreadNum(4);
    //rpc服务端准备启动，打印信息
    std::cout<<" RpcProvider start service at ip "<<ip<<" port: "<<port<<std::endl;

    //启动网络服务
    m_muduo_server->start();
    m_eventLoop.loop(); //开启事件循环
  /*
  这段代码是在启动网络服务和事件循环，其中server是一个TcpServer对象，m_eventLoop是一个EventLoop对象。
    首先调用server.start()函数启动网络服务。在Muduo库中，TcpServer类封装了底层网络操作
    包括TCP连接的建立和关闭、接收客户端数据、发送数据给客户端等等。通过调用TcpServer对象的start函数
    可以启动底层网络服务并监听客户端连接的到来。
    接下来调用m_eventLoop.loop()函数启动事件循环。在Muduo库中，EventLoop类封装了事件循环的核心逻辑
    包括定时器、IO事件、信号等等。通过调用EventLoop对象的loop函数，可以启动事件循环，等待事件的到来并处理事件。
    在这段代码中，首先启动网络服务，然后进入事件循环阶段，等待并处理各种事件。网络服务和事件循环是两个相对独立的模块
    它们的启动顺序和调用方式都是确定的。启动网络服务通常是在事件循环之前，因为网络服务是事件循环的基础。
    启动事件循环则是整个应用程序的核心，所有的事件都在事件循环中被处理。
  */
}

//新的连接的时候的回调函数

void RpcProvider::OnConnection(const muduo::net::TcpConnectionPtr & conn)
{
    //可以理解为在创建连接的时候出现了问题，导致没有连接上，而后触发这个回调函数把一些东西给销毁？
    if(!conn->connected() ){
        conn->shutdown();
    }
}

// 已建立连接用户的读写事件回调 如果远程有一个rpc服务的调用请求，那么OnMessage方法就会响应
// 这里来的肯定是一个远程调用请求
// 因此本函数需要：解析请求，根据服务名，方法名，参数，来调用service的来callmethod来调用本地的业务
void RpcProvider::OnMessage(const muduo::net::TcpConnectionPtr &conn, muduo::net::Buffer *buffer, muduo::Timestamp)
{
    //接收数据流(提取缓冲区的所有数据然后并返回一个字符串)
    std::string recv_buf=buffer->retrieveAllAsString();

    //使用protobuf的CodedInputStream来解析数据流
    google::protobuf::io::ArrayInputStream array_input(recv_buf.data(), recv_buf.size());
    google::protobuf::io::CodedInputStream coded_input(&array_input);
    uint32_t header_size{};

    coded_input.ReadVarint32(&header_size);  // 这里先设置读取的类型，是int32类型的

    // 根据header_size读取数据头的原始字符流，反序列化数据，得到rpc请求的详细信息
    std::string rpc_header_str;
    RPC::RpcHeader rpcHeader;
    std::string service_name;
    std::string method_name;

// 解析header_size（防止读取多余的数据，即防止读到请求体的东西什么的，正确保证读取到头的信息）
    google::protobuf::io::CodedInputStream::Limit msg_limit = coded_input.PushLimit(header_size);
    coded_input.ReadString(&rpc_header_str,header_size);
    //恢复之前的限制，以便安全地继续读取其他数据,即它可以读取超过head_size的大小了，即可以读取请求体部分内容了

    coded_input.PopLimit(msg_limit);
    uint32_t args_size{};

    //开始反序列化(主要将头部的一些信息拿出来)
    if(rpcHeader.ParseFromString(rpc_header_str) ){
        service_name=rpcHeader.service_name();
        method_name = rpcHeader.method_name();
        args_size = rpcHeader.args_size();
    }else{
            // 数据头反序列化失败
        std::cout << "rpc_header_str:" << rpc_header_str << " parse error!" << std::endl;
        return;
    }
    // 然后利用长度拿取请求体的内容
    std::string args_str;
    // 直接读取args_size长度的字符串数据(注意，此时这个还没有序列化)
    bool read_args_success = coded_input.ReadString(&args_str, args_size);

    if(!read_args_success){
        std::cout<<"read data fail"<<std::endl;
        return ;
    }
      // 获取service对象和method对象
    auto it = m_serviceMap.find(service_name);
    if (it == m_serviceMap.end()) {
        std::cout << "服务：" << service_name << " is not exist!" << std::endl;
        std::cout << "当前已经有的服务列表为:";
        for (auto item : m_serviceMap) {
        std::cout << item.first << " ";
        }
        std::cout << std::endl;
        return;
    }
    //找对应服务中某一个具体的方法
    auto mit = it->second.m_methodMap.find(method_name);
    if (mit == it->second.m_methodMap.end()) {
        std::cout << service_name << ":" << method_name << " is not exist!" << std::endl;
        return;
    }
    google::protobuf::Service *service = it->second.m_service;       // 获取service对象  new UserService
    const google::protobuf::MethodDescriptor *method = mit->second;  // 获取method对象  Login

    //封装一个全新的request,具有数据的隔离性：这样有时候可以做到线程安全，且如果使用原本的request，那么可能
    //它会通过此函数被污染
    google::protobuf::Message *request = service->GetRequestPrototype(method).New();
    if (!request->ParseFromString(args_str)) {
        std::cout << "request parse error, content:" << args_str << std::endl;
        return;
    }
    google::protobuf::Message *response = service->GetResponsePrototype(method).New();


    // 给下面的method方法的调用，绑定一个Closure的回调函数
    // closure(翻译为关闭，可能是销毁一些内容)是执行完本地方法之后会发生的回调，因此需要完成序列化和反向发送请求的操作
    google::protobuf::Closure *done =
    google::protobuf::NewCallback<RpcProvider, const muduo::net::TcpConnectionPtr &, google::protobuf::Message *>(
    this, &RpcProvider::SendRpcResponse, conn, response);

    //真正调用方法
    service->CallMethod(method, nullptr, request, response, done);
}

void RpcProvider::SendRpcResponse(const muduo::net::TcpConnectionPtr &conn, google::protobuf::Message *response )
{

    //响应回去的数据(这里主要是别人调用了你这OnMessage函数，然后用来响应回去的)
    std::string response_str;
    if(response->SerializeToString(&response_str) ) //进行序列化操作(放到对应字符串中)   
    {
        //然后将序列化的结果发送到rpc调用方
        conn->send(response_str);
    }else {
        std::cout << "serialize response_str error!" << std::endl;
    }

}

RpcProvider::~RpcProvider()
{
    //打印日志，说明该服务停止了
    std::cout << "[func - RpcProvider::~RpcProvider()]: ip和port信息：" << m_muduo_server->ipPort() << std::endl;
    m_eventLoop.quit();
}