#include "include/kvServer.h"

#include <rpcprovider.h>

#include "mprpcconfig.h"


void KvServer::DprintfKVDB() {
    if (!Debug) {
        return;
    }
    std::lock_guard<std::mutex> lg(m_mtx);
    DEFER {
        m_skipList.display_list();
    };
}

void KvServer::ExecuteAppendOpOnKVDB(Op op) 
{
    m_mtx.lock();

    m_skipList.insert_set_element(op.Key, op.Value);
    m_lastRequestId[op.ClientId] = op.RequestId;

    m_mtx.unlock();
    DprintfKVDB();
}

void KvServer::ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist) {
  m_mtx.lock();
  *value = "";
  *exist = false;
  if (m_skipList.search_element(op.Key, *value)) {
    *exist = true;
  }
  m_lastRequestId[op.ClientId] = op.RequestId;
  m_mtx.unlock();

  DprintfKVDB();
}

void KvServer::ExecutePutOpOnKVDB(Op op)
{
    m_mtx.lock();
    m_skipList.insert_set_element(op.Key, op.Value);
    m_lastRequestId[op.ClientId] = op.RequestId;
    m_mtx.unlock();

    DprintfKVDB();
}



void KvServer::PutAppend(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::PutAppendArgs *request,
                         ::raftKVRpcProctoc::PutAppendReply *response, ::google::protobuf::Closure *done) 
{
    KvServer::PutAppend(request, response);
    done->Run();
}

void KvServer::Get(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::GetArgs *request,
                   ::raftKVRpcProctoc::GetReply *response, ::google::protobuf::Closure *done)
{
    KvServer::Get(request, response);
    done->Run();
}


KvServer::KvServer(int me, int maxraftstate, std::string nodeInforFileName, short port) : m_skipList(10)
{
//    sleep(20);
    std::shared_ptr<Persister> persister = std::make_shared<Persister>(me); //待看使用
    m_me = me;
    //到什么时候需要进行日志快照
    m_maxRaftState = maxraftstate;
    //kvServer和raft节点的通信管道
    applyChan = std::make_shared<LockQueue<ApplyMsg> >();
    //clerk层面 kvserver开启rpc接受功能
    //    同时raft与raft节点之间也要开启rpc功能，因此有两个注册

    m_raftNode = std::make_shared<Raft>();


    //创建一个线程，注册一个rpc服务，以及启动这个rpc服务
    //固然阻塞之后，这个kvserver的其中一个线程将会进行端口的监听
      std::thread t([this, port]() -> void {
    // provider是一个rpc网络服务对象。把UserService对象发布到rpc节点上
    //创建一个注册中心
    RpcProvider provider;
    //将对应的这个kvServer以及raft的服务创建在这个provider中
    provider.NotifyService(this);
//    std::cout<<"notify servicd now"<<std::endl;
    provider.NotifyService(this->m_raftNode.get());  
        // 启动一个rpc服务发布节点   Run以后，进程进入阻塞状态，等待远程的rpc调用请求
        provider.Run(m_me, port);
    });
    t.detach();

    ////开启rpc远程调用能力，需要注意必须要保证所有节点都开启rpc接受功能之后才能开启rpc远程调用能力
    ////这里使用睡眠来保证
    ////即这边通过这个线程，开启了当前raft结点的tcpserver服务器，用来进行监听其他raft结点的请求，
    ////保证每个raft结点的服务都开启了，这样构造函数的下面才能与其他raft结点进行连接操作（后面才能进行远程通信，通过RPCUTIL这个类）
    std::cout << "raftServer node:" << m_me << " start to sleep to wait all ohter raftnode start!!!!" << std::endl;
    sleep(6);
    std::cout << "raftServer node:" << m_me << " wake up!!!! start to connect other raftnode" << std::endl;
    //获取所有raft节点ip、port ，并进行连接  ,要排除自己
    MprpcConfig config;
    //nodeInforFileName这个其实就是针对test.conf这个配置文件
    //这个配置文件包括了其他raft结点的信息
    config.LoadConfigFile(nodeInforFileName.c_str());
    std::vector<std::pair<std::string, short> > ipPortVt;
    for (int i = 0; i < INT_MAX - 1; ++i) {
        std::string node = "node" + std::to_string(i);
        std::string nodeIp = config.Load(node + "ip");
        std::string nodePortStr = config.Load(node + "port");
        if (nodeIp.empty()) {
            break;
        }
        ipPortVt.emplace_back(nodeIp, atoi(nodePortStr.c_str())); 
    }
    //开始进行连接：RaftRpcUtil内部会有一个stub，创建通道的
    std::vector<std::shared_ptr<RaftRpcUtil> > servers;
    for (int i = 0; i < ipPortVt.size(); ++i) {
        //不和自己连接
        if (i == m_me) {
            servers.push_back(nullptr);
            continue;
        }
        std::string otherNodeIp = ipPortVt[i].first;
        short otherNodePort = ipPortVt[i].second;
        //每一个kvserver都有多个RaftRpcUtil，每一个RaftRpcUtil内部都有一个channel通道，当我们使用RaftRpcUtil的时候进行对应方法的调用
        //GRPC内部会将channel的内部方法进行回调,即进行CallMethod调用
        auto *rpc = new RaftRpcUtil(otherNodeIp, otherNodePort);
        servers.push_back(std::shared_ptr<RaftRpcUtil>(rpc));

        std::cout << "node" << m_me << " 连接node" << i << "success!" << std::endl;
    }
    sleep(ipPortVt.size() - me);  //等待所有节点相互连接成功，再启动raft
    m_raftNode->init(servers, m_me, persister, applyChan); //applyChan是和raft结点的通道，大概就是在raft结点中往kvserver这个地方存数据，然后kvserver再进行一个持久化？
    //  // kv的server直接与raft通信，但kv不直接与raft通信，所以需要把ApplyMsg的chan传递下去用于通信，两者的persist也是共用的

    m_skipList;
    waitApplyCh;
    m_lastRequestId;
    m_lastSnapShotRaftLogIndex = 0; 
    auto snapshot = persister->ReadSnapshot();
    if (!snapshot.empty()) {
        ReadSnapShotToInstall(snapshot);
    }
    std::thread t2(&KvServer::ReadRaftApplyCommandLoop, this);  //马上向其他节点宣告自己就是leader
    t2.join();  //由于ReadRaftApplyCommandLoop一直不会結束，达到一直卡在这的目的
    //这样做应该是为了减少一个线程的开销或者多开一个专门的函数，但是感觉不太优雅这么写
}


//（具体还得细看）
// raft会与persist层交互，kvserver层也会，因为kvserver层开始的时候需要恢复kvdb的状态
//  关于快照raft层与persist的交互：保存kvserver传来的snapshot；生成leaderInstallSnapshot RPC的时候也需要读取snapshot；
//  因此snapshot的具体格式是由kvserver层来定的，raft只负责传递这个东西
//  snapShot里面包含kvserver需要维护的persist_lastRequestId 以及kvDB真正保存的数据persist_kvdb
void KvServer::ReadSnapShotToInstall(std::string snapshot) 
{
    if (snapshot.empty()) {
        return;
    }
    parseFromString(snapshot);
}

//永久循环部分，从kvServer和raft节点的通信管道中拿取数据
//然后执行命令，比如说执行对应的插入到kv键值的跳表或者更改的操作
void KvServer::ReadRaftApplyCommandLoop()
{
    while(true){
        //applyChan自己带锁
        auto message =applyChan->Pop(); //阻塞弹出
        DPrintf("---------------tmp-------------[func-KvServer::ReadRaftApplyCommandLoop()-kvserver{%d}] 收到了下raft的消息",m_me);
        if (message.CommandValid) {
            GetCommandFromRaft(message);
        }
        if (message.SnapshotValid) {
            GetSnapShotFromRaft(message);
        }
    }
}

void KvServer::GetSnapShotFromRaft(ApplyMsg message)
{
    std::lock_guard<std::mutex> lg(m_mtx);
    if (m_raftNode->CondInstallSnapshot(message.SnapshotTerm, message.SnapshotIndex, message.Snapshot)) {
        ReadSnapShotToInstall(message.Snapshot);
        m_lastSnapShotRaftLogIndex = message.SnapshotIndex;
    }
}



void KvServer::GetCommandFromRaft(ApplyMsg message) 
{
    //将命令解析为字符串形式
    Op op;
    op.parseFromString(message.Command);

    DPrintf(
          "[KvServer::GetCommandFromRaft-kvserver{%d}] , Got Command --> Index:{%d} , ClientId {%s}, RequestId {%d}, "
            "Opreation {%s}, Key :{%s}, Value :{%s}",
            m_me, message.CommandIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);    

    //这个应该是快照的部分，不是日志的部分，固然得return
    //换句话说，这个日志已经被保存过了，而且形式还是以快照的形式来保存的
    if (message.CommandIndex <= m_lastSnapShotRaftLogIndex) {
        return;
    }
    // 状态机（KVServer解决重复问题）
    // 不会执行重复的命令
    //如果不重复的命令，那么就执行
    if (!ifRequestDuplicate(op.ClientId, op.RequestId)) {
        if (op.Operation == "Put") {
            ExecutePutOpOnKVDB(op);
        }
        if (op.Operation == "Append") {
            ExecuteAppendOpOnKVDB(op);
        }
    }
    ////到这里kvDB已经制作了快照
    ////m_maxRaftState这个表示多少日志的数量要进行快照
    if (m_maxRaftState != -1) {
        //判断是否需要快照，若要，内部会直接进行对应的操作
        IfNeedToSendSnapShotCommand(message.CommandIndex, 9);  //这个9感觉一点用都没有
        //如果raft的log太大（大于指定的比例）就把制作快照
    }
    SendMessageToWaitChan(op, message.CommandIndex);
}

bool KvServer::ifRequestDuplicate(std::string ClientId, int RequestId)
{
    std::lock_guard<std::mutex> lg(m_mtx);
    //说明这个用户本来就不存在，那么肯定返回false
    if (m_lastRequestId.find(ClientId) == m_lastRequestId.end()) {
        return false;
    }
    //查看是否之前被请求过的记录下标,判断是否是重复请求
    return RequestId <= m_lastRequestId[ClientId];
}

void KvServer::IfNeedToSendSnapShotCommand(int raftIndex, int proportion)
{
    if (m_raftNode->GetRaftStateSize() > m_maxRaftState / 10.0) {
        auto snapshot = MakeSnapShot(); //制作快照
        m_raftNode->Snapshot(raftIndex, snapshot); //制作一个快照，然后将其raftindex位置前的日志都删除，且存放快照
    }
}

std::string KvServer::MakeSnapShot() {
    std::lock_guard<std::mutex> lg(m_mtx);
    std::string snapshotData = getSnapshotData(); //在keserver.h头文件中
    ////这边返回了对应序列化后的数据，比如当前kvserver的跳表（即文件中的数据）中的数据，当前kvserver的类（即这个类里面的所有数据）以及
    ////raft结点与其他结点通信的数据（下一次应该往其他raft发送的数据下标应为多少）
    return snapshotData;
}

bool KvServer::SendMessageToWaitChan(const Op &op, int raftIndex)
{
    std::lock_guard<std::mutex> lg(m_mtx);
    DPrintf(
            "[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
            "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
            m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

    if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
        return false;
    }
    waitApplyCh[raftIndex]->Push(op);
    DPrintf(
            "[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
            "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
            m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
    return true; 
}

// 处理来自clerk的Get RPC
void KvServer::Get(const raftKVRpcProctoc::GetArgs *args, raftKVRpcProctoc::GetReply *reply)
{
    Op op;
    op.Operation = "Get";
    op.Key = args->key();
    op.Value = "";
    op.ClientId = args->clientid();
    op.RequestId = args->requestid();

    int raftIndex = -1;
    int _ = -1;
    bool isLeader = false;
    //获取新的日志下标的位置，应该被放到哪里
    m_raftNode->Start(op, &raftIndex, &_,&isLeader); 

    if (!isLeader) {
        reply->set_err(ErrWrongLeader);
        return;
    }
    // create waitForCh
    m_mtx.lock();
    if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
        //构造一个日志队列，专门为这个raftIndex
        waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];

    m_mtx.unlock();  //直接解锁，等待任务执行完成，不能一直拿锁等待

    // timeout
    Op raftCommitOp;

    if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) {
        //如果当前超时了，但是之前成功提交过，那么我们也可以认为他是成功的
        int _ = -1;
        bool isLeader = false;
        m_raftNode->GetState(&_, &isLeader);
        if (ifRequestDuplicate(op.ClientId, op.RequestId) && isLeader){
            //如果超时，代表raft集群不保证已经commitIndex该日志，但是如果是已经提交过的get请求，是可以再执行的。
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist);
            if (exist) {
                reply->set_err(OK);
                reply->set_value(value);
            }else{
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }      
        
        
        }else{ //否则可能不是领导者，
            reply->set_err(ErrWrongLeader);  //返回这个，其实就是让clerk换一个节点重试
        }
    }else{ //说明 raft已经提交了该command（op），可以正式开始执行了
        if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId) {
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist);
            if (exist) {
                reply->set_err(OK);
                reply->set_value(value);
            } else {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        }else{
            reply->set_err(ErrWrongLeader);
        }
        
    }
    m_mtx.lock(); 
    auto tmp = waitApplyCh[raftIndex];
    waitApplyCh.erase(raftIndex);
    delete tmp;
    m_mtx.unlock();

}


void KvServer::PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply)
{
    Op op;
    op.Operation = args->op();
    op.Key = args->key();
    op.Value = args->value();
    op.ClientId = args->clientid();
    op.RequestId = args->requestid();
    int raftIndex = -1;
    int _ = -1;
    bool isleader = false;

    m_raftNode->Start(op, &raftIndex, &_, &isleader);


    if (!isleader) {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , but "
            "not leader",
            m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);

        reply->set_err(ErrWrongLeader);
        return;
    }

    DPrintf(
        "[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , is "
        "leader ",
        m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);
    m_mtx.lock();
    if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
        //构造一个日志队列，专门为这个raftIndex
        waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];
    m_mtx.unlock();  //直接解锁，等待任务执行完成，不能一直拿锁等待
    // timeout
    Op raftCommitOp;

    if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]TIMEOUT PUTAPPEND !!!! Server %d , get Command <-- Index:%d , "
            "ClientId %s, RequestId %s, Opreation %s Key :%s, Value :%s",
            m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

        if (ifRequestDuplicate(op.ClientId, op.RequestId)) {
            reply->set_err(OK);  // 超时了,但因为是重复的请求，返回ok，实际上就算没有超时，在真正执行的时候也要判断是否重复
        } else {
            reply->set_err(ErrWrongLeader);  ///这里返回这个的目的让clerk重新尝试
        }
  } else {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]WaitChanGetRaftApplyMessage<--Server %d , get Command <-- Index:%d , "
            "ClientId %s, RequestId %d, Opreation %s, Key :%s, Value :%s",
            m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
        if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId) {
        //可能发生leader的变更导致日志被覆盖，因此必须检查
            reply->set_err(OK);
        } else {
            reply->set_err(ErrWrongLeader);
        }
  }
  m_mtx.lock();
  auto tmp = waitApplyCh[raftIndex];
  waitApplyCh.erase(raftIndex);
  delete tmp;
  m_mtx.unlock();

}


