#include "raft.h"
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <memory>
#include "config.h"
#include "util.h"

int Raft::GetRaftStateSize() 
{
    return m_persister->RaftStateSize(); 
}


//作为接收方的方法
void Raft::AppendEntries1(const raftRpcProctoc::AppendEntriesArgs* args, raftRpcProctoc::AppendEntriesReply* reply){
    std::lock_guard<std::mutex> locker(m_mtx);
    reply->set_appstate(AppNormal); //设置当前网络正常，因为可以到达当前结点
    //如果领导者的任期小于追随者
    if (args->term() < m_currentTerm) {
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(-100);  // 这个-100设置感觉没啥用，因为发送方的逻辑当领导者的任期比当前结点要小的时候，只会更新当前任期以及变成追随者
        DPrintf("[func-AppendEntries-rf{%d}] 拒绝了 因为Leader{%d}的term{%v}< rf{%d}.term{%d}\n", m_me, args->leaderid(),
                args->term(), m_me, m_currentTerm);
        return;  // 注意从过期的领导人收到消息不要重设定时器
  }
    //下面这个不太理解：为何需要延迟调用persist（）函数
    DEFER{
        persist();
    };
    //将自己的任期更新
    if(args->term() > m_currentTerm){
        m_status=Follower;
        m_currentTerm=args->term();
        m_votedFor=-1;      //设置-1是有意义的，因为突然舵机也是可以投票的
    }
     myAssert(args->term() == m_currentTerm, format("assert {args.Term == rf.currentTerm} fail"));
    //如果发生网络分区，那么候选者可能会收到同一个term的leader的消息，要转变你为follower(查看文档2)
    m_status=Follower;
    m_lastResetElectionTime=now();

    //比较日志（当任期相同的时候需要比较日志）
    if(args->prevlogindex() >getLastLogIndex() ){
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(getLastLogIndex() + 1);
        return;
    }else if(args->prevlogindex() < m_lastSnapshotIncludeIndex){
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(m_lastSnapshotIncludeIndex+1);
    }
    //if部分开始，首先判断当前
    if (matchLog(args->prevlogindex(), args->prevlogterm())){
        //for循环开始
        for(int i=0;i<args->entries_size();i++){
            auto log=args->entries(i);
            if(log.logindex() > getLastLogIndex()){
                //超过就添加日志，一般来说，都是会进入这里的
                m_logs.push_back(log);
            }else{
                //没超过就比较是否匹配，不匹配再更新，而不是直接截断
                if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() == log.logterm() &&
                    m_logs[getSlicesIndexFromLogIndex(log.logindex())].command() != log.command()) {
                        //相同位置的log ，其logTerm相等，但是命令却不相同，不符合raft的前向匹配，异常了！
                        myAssert(false, format("[func-AppendEntries-rf{%d}] 两节点logIndex{%d}和term{%d}相同，但是其command{%d:%d}   "
                                        " {%d:%d}却不同！！\n",
                                         m_me, log.logindex(), log.logterm(), m_me,
                                        m_logs[getSlicesIndexFromLogIndex(log.logindex())].command(), args->leaderid(),
                                        log.command()));
                }
                //不匹配就更新
                if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() != log.logterm()) {
                    m_logs[getSlicesIndexFromLogIndex(log.logindex())] = log ;
                }
        }
    } 
    //for循环结束
    myAssert(getLastLogIndex() >= args->prevlogindex() + args->entries_size(),
                format("[func-AppendEntries1-rf{%d}]rf.getLastLogIndex(){%d} != args.PrevLogIndex{%d}+len(args.Entries){%d}",
                m_me, getLastLogIndex(), args->prevlogindex(), args->entries_size()));
    // m_commitIndex表示要更新到状态机的索引部分:这个应该和defer的函数有关了，至于为什么取小，以及为什么领导者提交日志可能要小于
    //追随者所提交到状态机的日志，看文档2
    if(args->leadercommit() > m_commitIndex){
         m_commitIndex = std::min(args->leadercommit(), getLastLogIndex());
    }
    myAssert(getLastLogIndex() >= m_commitIndex,
                 format("[func-AppendEntries1-rf{%d}]  rf.getLastLogIndex{%d} < rf.commitIndex{%d}", m_me,
                        getLastLogIndex(), m_commitIndex));
    reply->set_success(true);
    reply->set_term(m_currentTerm);
    return ;
    }
    //进入到else部分：
    // PrevLogIndex 长度合适，但是不匹配，因此往前寻找 矛盾的term的第一个元素
    // 为什么该term的日志都是矛盾的呢？也不一定都是矛盾的，只是这么优化减少rpc而已
    // ？什么时候term会矛盾呢？很多情况，比如leader接收了日志之后马上就崩溃等等
    reply->set_updatenextindex(args->prevlogindex());

    for (int index = args->prevlogindex(); index >= m_lastSnapshotIncludeIndex; --index) {
        if (getLogTermFromLogIndex(index) != getLogTermFromLogIndex(args->prevlogindex())) {
            reply->set_updatenextindex(index + 1);
            break;
        }
    }
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    return ;
}

//作为发送方的代码：
bool Raft::sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                             std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply,
                             std::shared_ptr<int> appendNums)
{
    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc開始 ， args->entries_size():{%d}", m_me,
          server, args->entries_size());
    //向当前server发送对应信息
    //这个ok是网络是否正常通信的ok，而不是requestVote rpc是否投票的rpc
    //通过调用raftRpcUtil这个类里面的方法，帮助我们调用raft类里面的AppendEntries->AppendEntries1方法进行真正发送
    //当前的控制器是维护在raftRpcUtil类里面的，解耦合的操作
   
    bool ok = m_peers[server]->AppendEntries(args.get(), reply.get());
    //如果网络有问题
    //发送的时候是阻塞的，固然走到下一步表明发送完毕，并且接收响应完毕
    if(!ok){
        DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc失敗", m_me, server);
        return ok;
    }
    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc成功", m_me, server);
    if (reply->appstate() == Disconnected) { //appstate标识结点网络状态的
        return ok;
    }

    std::lock_guard<std::mutex> lg1(m_mtx);

    //对reply进行处理
    // 对于rpc通信，无论什么时候都要检查term
    if(reply->term() > m_currentTerm ){
        m_status=Follower;
        m_currentTerm=reply->term();   
        //更新当前追随者的任期（假设网络分区，领导者被分到少数结点部分，那么多数结点部分会分到另外一个地方
        //则此时多数结点会重新选举，使得当前领导者任期要小于另外一个领导者，固然主动降级
        m_votedFor=-1;
        return ok;
    }else if (reply->term() < m_currentTerm) {
        DPrintf("[func -sendAppendEntries  rf{%d}]  节点：{%d}的term{%d}<rf{%d}的term{%d}\n", m_me, server, reply->term(),
                m_me, m_currentTerm);
        return ok;
    }

    if (m_status != Leader) {
        //如果不是leader，那么就不要对返回的情况进行处理了
        //因为是循环调用的，固然当出现if(reply->term() > m_currentTerm)的时候，会return，以至于就不是领导者了，当循环要发送信息到下一次对象
        //的时候，发现他不是领导者了，就不能做下面处理了,此时会进入到接收方的 if (args->term() < m_currentTerm)部分：直接进行return false
        return ok;
    }

    //说明任期term相同
    myAssert(reply->term() == m_currentTerm,
           format("reply.Term{%d} != rf.currentTerm{%d}   ", reply->term(), m_currentTerm));
    //说明任期相同但是日志对应任期不匹配的情况，matchLog返回false的情况
    if (!reply->success()) {
        //这个应该永远都不为-100吧，任期相同，我看响应方就没有设置-100的时候
        if(reply->updatenextindex() != -100){
            DPrintf("[func -sendAppendEntries  rf{%d}]  返回的日志term相等，但是不匹配，回缩nextIndex[%d]：{%d}\n", m_me,
                    server, reply->updatenextindex());
            m_nextIndex[server] = reply->updatenextindex();  //失败是不更新mathIndex的
        }

    }else{
        //表示成功的追随者+1，用来查看是否多数结点成功的标志
        *appendNums = *appendNums + 1;
        DPrintf("---------------------------tmp------------------------- 節點{%d}返回true,當前*appendNums{%d}", server,
                *appendNums);
        m_matchIndex[server] = std::max(m_matchIndex[server], args->prevlogindex() + args->entries_size()); //这个具体看文档2
         m_nextIndex[server] = m_matchIndex[server] + 1; //下一次更新的索引+1
        int lastLogIndex = getLastLogIndex();
        //断言解析：正常情况下，不出现任何网络分区等错误的时候，会是等于的，在上面使得m_nextIndex+1,固然这里lastLogIndex+1也会等于
        //即追随者和领导者的下一次更新日志都会相同
        myAssert(m_nextIndex[server] <= lastLogIndex + 1,
            format("error msg:rf.nextIndex[%d] > lastLogIndex+1, len(rf.logs) = %d   lastLogIndex{%d} = %d", server,
                m_logs.size(), server, lastLogIndex));
        //说明可以直接提交了
        if (*appendNums >= 1 + m_peers.size() / 2) {
            *appendNums = 0;
            //打个日志
            if (args->entries_size() > 0) {
                DPrintf("args->entries(args->entries_size()-1).logterm(){%d}   m_currentTerm{%d}",
                    args->entries(args->entries_size() - 1).logterm(), m_currentTerm);
            }
            //如果当前发送的日志>0并且当前日志的任期跟m_currentTerm相同
            if (args->entries_size() > 0 && args->entries(args->entries_size() - 1).logterm() == m_currentTerm) {
                DPrintf(
                "---------------------------tmp------------------------- 當前term有log成功提交，更新leader的m_commitIndex "
                "from{%d} to{%d}",
                m_commitIndex, args->prevlogindex() + args->entries_size());
                //更新m_commitIndex，m_commitIndex表示哪些日志被提交了
                m_commitIndex = std::max(m_commitIndex, args->prevlogindex() + args->entries_size());
            }
            myAssert(m_commitIndex <= lastLogIndex,
                format("[func-sendAppendEntries,rf{%d}] lastLogIndex:%d  rf.commitIndex:%d\n", m_me, lastLogIndex,
                        m_commitIndex));
        }
    }
    return ok;
}


void Raft::getLastLogIndexAndTerm(int * lastLogIndex,int *lastLogTerm){

    if(m_logs.empty()){
        *lastLogIndex = m_lastSnapshotIncludeIndex;
        *lastLogTerm = m_lastSnapshotIncludeTerm;
    }else {
        *lastLogIndex = m_logs[m_logs.size() - 1].logindex();
        *lastLogTerm = m_logs[m_logs.size() - 1].logterm();
    }

}


//这里第二个参数不关心，只关心当前的日志的index
int Raft::getLastLogIndex(){
    int lastLogIndex =-1;
    int _=-1;
    getLastLogIndexAndTerm(&lastLogIndex,&_);
    return lastLogIndex;
}
//与上面同理
int Raft::getLastLogTerm(){

    int _=-1;
    int lastLogTerm=-1;
    getLastLogIndexAndTerm(&_,&lastLogTerm);
    return lastLogTerm;
}

//重写基类方法部分：
void Raft::AppendEntries(google::protobuf::RpcController* controller,
                         const ::raftRpcProctoc::AppendEntriesArgs* request,
                         ::raftRpcProctoc::AppendEntriesReply* response, ::google::protobuf::Closure* done) {
  AppendEntries1(request, response);
  done->Run();
}

void Raft::InstallSnapshot(google::protobuf::RpcController* controller,
                           const ::raftRpcProctoc::InstallSnapshotRequest* request,
                           ::raftRpcProctoc::InstallSnapshotResponse* response, ::google::protobuf::Closure* done) {
  InstallSnapshot(request, response);

  done->Run();
}

void Raft::RequestVote(google::protobuf::RpcController* controller, const ::raftRpcProctoc::RequestVoteArgs* request,
                       ::raftRpcProctoc::RequestVoteReply* response, ::google::protobuf::Closure* done) {
  RequestVote(request, response);
  done->Run();
}
////重写基类方法结束部分

//获取当前日志下标所对应在这个数组的位置
int Raft::getSlicesIndexFromLogIndex(int logIndex)
{
    myAssert(logIndex > m_lastSnapshotIncludeIndex,
           format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} <= rf.lastSnapshotIncludeIndex{%d}", m_me,
                  logIndex, m_lastSnapshotIncludeIndex));
    int lastLogIndex=getLastLogIndex();
     myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                                            m_me, logIndex, lastLogIndex));
    int SliceIndex=logIndex - m_lastSnapshotIncludeIndex-1;
    return SliceIndex;
}

int Raft::getLogTermFromLogIndex(int logIndex)
{
    myAssert(logIndex >= m_lastSnapshotIncludeIndex,
        format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} < rf.lastSnapshotIncludeIndex{%d}", m_me,
                logIndex, m_lastSnapshotIncludeIndex));
    int lastLogIndex=getLastLogIndex();
    myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                                        m_me, logIndex, lastLogIndex));
    //如果当前的日志本是快照的最后一个部分，那么就不用耗费函数去查找，直接返回了
    //获取该日志下标所对应的任期是在哪里
    if(logIndex == m_lastSnapshotIncludeIndex){
        return m_lastSnapshotIncludeTerm;
    }
    return m_logs[getSlicesIndexFromLogIndex(logIndex)].logterm();
}

bool Raft::matchLog(int logIndex,int logTerm){
    myAssert(logIndex >= m_lastSnapshotIncludeIndex && logIndex <= getLastLogIndex(),
        format("不满足：logIndex{%d}>=rf.lastSnapshotIncludeIndex{%d}&&logIndex{%d}<=rf.getLastLogIndex{%d}",
                logIndex, m_lastSnapshotIncludeIndex, logIndex, getLastLogIndex()));
    //查看当前日志所对应的任期是否相同
    return logTerm==getLogTermFromLogIndex(logIndex);
}

void Raft::doHeartBeat()
{
    std::lock_guard<std::mutex> g(m_mtx);
    if(m_status != Leader)
        return ;
    DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了且拿到mutex，开始发送AE\n", m_me);
    auto appendNums = std::make_shared<int>(1);  //正确返回的节点的数量,从一开始表示已经发给了自己
    for(int i=0;i<m_peers.size();i++){
        if(i==m_me)
            continue;
        DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了 index:{%d}\n", m_me, i);
        myAssert(m_nextIndex[i] >= 1, format("rf.nextIndex[%d] = {%d}", i, m_nextIndex[i]));
        //判断是否需要发送快照，如果追随者的部分小于领导者的快照部分，那么就发送快照，因为之前的日志已经被删除了
        if(m_nextIndex[i]<=m_lastSnapshotIncludeIndex){
            std::thread t(&Raft::leaderSendSnapShot, this, i);  // 创建新线程并执行b函数，并传递参数
            t.detach();
            continue;
        }
        //发送日志：
        int preLogIndex = -1;
        int PrevLogTerm = -1;
        //获取当前领导者应该从哪一个位置开始发对应的日志给到某一个追随者
        getPrevLogInfo(i, &preLogIndex, &PrevLogTerm);
        std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> appendEntriesArgs =
        std::make_shared<raftRpcProctoc::AppendEntriesArgs>();
        appendEntriesArgs->set_term(m_currentTerm);
        appendEntriesArgs->set_leaderid(m_me);
        appendEntriesArgs->set_prevlogindex(preLogIndex);
        appendEntriesArgs->set_prevlogterm(PrevLogTerm);
        appendEntriesArgs->clear_entries();
        appendEntriesArgs->set_leadercommit(m_commitIndex);
        if (preLogIndex != m_lastSnapshotIncludeIndex) {
            for (int j = getSlicesIndexFromLogIndex(preLogIndex) + 1; j < m_logs.size(); ++j) {
                raftRpcProctoc::LogEntry* sendEntryPtr = appendEntriesArgs->add_entries();
                *sendEntryPtr = m_logs[j];  //=是可以点进去的，可以点进去看下protobuf如何重写这个的
            }
        } else {
            for (const auto& item : m_logs) {
                raftRpcProctoc::LogEntry* sendEntryPtr = appendEntriesArgs->add_entries();
                *sendEntryPtr = item;  //=是可以点进去的，可以点进去看下protobuf如何重写这个的
            }
        }
        int lastLogIndex = getLastLogIndex();
        // leader对每个节点发送的日志长短不一，但是都保证从prevIndex发送直到最后
        myAssert(appendEntriesArgs->prevlogindex() + appendEntriesArgs->entries_size() == lastLogIndex,
                   format("appendEntriesArgs.PrevLogIndex{%d}+len(appendEntriesArgs.Entries){%d} != lastLogIndex{%d}",
                          appendEntriesArgs->prevlogindex(), appendEntriesArgs->entries_size(), lastLogIndex));
        //构造返回值：
        const std::shared_ptr<raftRpcProctoc::AppendEntriesReply> appendEntriesReply =
        std::make_shared<raftRpcProctoc::AppendEntriesReply>();
        appendEntriesReply->set_appstate(Disconnected);
        std::thread t(&Raft::sendAppendEntries, this, i, appendEntriesArgs, appendEntriesReply,
                        appendNums);  // 创建新线程并执行b函数，并传递参数
        t.detach();
    }
    m_lastResetHearBeatTime = now();  //更新心跳时间
}

void Raft::getPrevLogInfo(int server, int* preIndex, int* preTerm)
{
    //m_lastSnapshotIncludeIndex最后一个快照的下标其实就是日志的第一个下标之处
    //通过前置判断可以减少getSlicesIndexFromLogIndex的函数调用，其实直接下面那个也没问题
    if (m_nextIndex[server] == m_lastSnapshotIncludeIndex + 1) {
        //要发送的日志是第一个日志，因此直接返回m_lastSnapshotIncludeIndex和m_lastSnapshotIncludeTerm
        *preIndex = m_lastSnapshotIncludeIndex;
        *preTerm = m_lastSnapshotIncludeTerm;
        return;
    }
    auto nextIndex = m_nextIndex[server];
    *preIndex = nextIndex - 1;
    *preTerm = m_logs[getSlicesIndexFromLogIndex(*preIndex)].logterm();
}


void Raft::InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest* args,
                           raftRpcProctoc::InstallSnapshotResponse* reply) 
{
    m_mtx.lock();
    DEFER { m_mtx.unlock(); };
    if (args->term() < m_currentTerm) {
        reply->set_term(m_currentTerm);
        return;
    }
    if (args->term() > m_currentTerm) {
        //后面两种情况都要接收日志
        m_currentTerm = args->term();
        m_votedFor = -1;
        m_status = Follower;
        persist();
    }
    m_status = Follower;
    m_lastResetElectionTime = now();
    //可能之前已经同步过快照了
    if (args->lastsnapshotincludeindex() <= m_lastSnapshotIncludeIndex) {
        return;
    }
    //由于现在保存快照了，固然需要对快照包含的范围内的日志进行一个清除
    auto lastLogIndex = getLastLogIndex();
    if (lastLogIndex > args->lastsnapshotincludeindex()) {
        m_logs.erase(m_logs.begin(), m_logs.begin() + getSlicesIndexFromLogIndex(args->lastsnapshotincludeindex()) + 1);
    } else {
        m_logs.clear();
    }
    m_commitIndex = std::max(m_commitIndex, args->lastsnapshotincludeindex());
    m_lastApplied = std::max(m_lastApplied, args->lastsnapshotincludeindex());
    m_lastSnapshotIncludeIndex = args->lastsnapshotincludeindex();
    m_lastSnapshotIncludeTerm = args->lastsnapshotincludeterm();
    reply->set_term(m_currentTerm);
    //这里还需要理解：
    ApplyMsg msg;
    msg.SnapshotValid = true;
    msg.Snapshot = args->data();
    msg.SnapshotTerm = args->lastsnapshotincludeterm();
    msg.SnapshotIndex = args->lastsnapshotincludeindex();
    applyChan->Push(msg);     //待删除：感觉不应该执行两次
    std::thread t(&Raft::pushMsgToKvServer, this, msg);  // 创建新线程并执行b函数，并传递参数
    t.detach();
    m_persister->Save(persistData(), args->data());

}

void Raft::pushMsgToKvServer(ApplyMsg msg) 
{
    applyChan->Push(msg); 
}

void Raft::leaderSendSnapShot(int server)
{
    m_mtx.lock();
    raftRpcProctoc::InstallSnapshotRequest args;
    args.set_leaderid(m_me);
    args.set_term(m_currentTerm);
    args.set_lastsnapshotincludeindex(m_lastSnapshotIncludeIndex);
    args.set_lastsnapshotincludeterm(m_lastSnapshotIncludeTerm);
    //数据即这个快照在persister文件部分
    args.set_data(m_persister->ReadSnapshot());
    raftRpcProctoc::InstallSnapshotResponse reply;
    m_mtx.unlock();
    //开始发送
    bool ok = m_peers[server]->InstallSnapshot(&args, &reply);
    m_mtx.lock();
    DEFER { m_mtx.unlock(); };
    if (!ok) {
        return;
    }

    if (m_status != Leader || m_currentTerm != args.term()) {
        return;  //中间释放过锁，可能状态已经改变了（特别细节的一个地方）m_peers[server]->InstallSnapshot(&args, &reply);可能在这个函数
        //被设置为了追随者
    }
    if (reply.term() > m_currentTerm) {
        //三变
        m_currentTerm = reply.term();
        m_votedFor = -1;
        m_status = Follower;
        persist();   //（具体逻辑得看这个文件）
        m_lastResetElectionTime = now();
        return;
    }
    // m_matchIndex表示当前追随者更新的日志索引处，跟当前领导者同步到了哪里
    //m_next表示下次同步到哪里
    m_matchIndex[server] = args.lastsnapshotincludeindex();
    m_nextIndex[server] = m_matchIndex[server] + 1;
}

bool Raft::CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot) 
{
    return true;
}

void Raft::doElection() 
{
    std::lock_guard<std::mutex> g(m_mtx);
    if (m_status == Leader)
        return ;
    DPrintf("[       ticker-func-rf(%d)              ]  选举定时器到期且不是leader，开始选举 \n", m_me);
    m_status = Candidate;
        ///开始新一轮的选举
    m_currentTerm += 1;
    m_votedFor = m_me;  //即是自己给自己投，也避免candidate给同辈的candidate投
    persist();
    std::shared_ptr<int> votedNum = std::make_shared<int>(1) ;
    m_lastResetElectionTime = now();
    for (int i = 0; i < m_peers.size(); i++) {
        if (i == m_me) {
            continue;
        }
        //发送的时候也要携带自己本身的日志，因为当任期相同的时候，用日志进行比较，查看是否投票
        int lastLogIndex = -1, lastLogTerm = -1;
        getLastLogIndexAndTerm(&lastLogIndex, &lastLogTerm);  //获取最后一个log的term和下标
        std::shared_ptr<raftRpcProctoc::RequestVoteArgs> requestVoteArgs =
        std::make_shared<raftRpcProctoc::RequestVoteArgs>();
        requestVoteArgs->set_term(m_currentTerm);
        requestVoteArgs->set_candidateid(m_me);
        requestVoteArgs->set_lastlogindex(lastLogIndex);
        requestVoteArgs->set_lastlogterm(lastLogTerm);
        auto requestVoteReply = std::make_shared<raftRpcProctoc::RequestVoteReply>();

        std::thread t(&Raft::sendRequestVote, this, i, requestVoteArgs, requestVoteReply,
                        votedNum);  // 创建新线程并执行b函数，并传递参数
        t.detach();
    }

}

bool Raft::sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                           std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum)
{
    auto start = now();
    DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 發送 RequestVote 開始", m_me, m_currentTerm, getLastLogIndex());
    bool ok = m_peers[server]->RequestVote(args.get(), reply.get());
    DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 發送 RequestVote 完畢，耗時:{%d} ms", m_me, m_currentTerm,
            getLastLogIndex(), now() - start);

    if (!ok) {
        return ok;  //不加这个的话如果服务器宕机会出现问题的
    }
    ///对回应进行处理，要记得无论什么时候收到回复就要检查term
    //  std::lock_guard<std::mutex> lg(m_mtx);
    if (reply->term() > m_currentTerm) {
        m_status = Follower;  //三变：身份，term，和投票
        m_currentTerm = reply->term();
        m_votedFor = -1;
        persist();
        return true;
    } else if (reply->term() < m_currentTerm) {
        return true;
    }
    myAssert(reply->term() == m_currentTerm, format("assert {reply.Term==rf.currentTerm} fail"));
    //如果投票不被通过
    if (!reply->votegranted()) {
        return true;
    }
    *votedNum = *votedNum + 1;
    if (*votedNum < m_peers.size() / 2 + 1)
        return true;
    //需要变成领导者了
    *votedNum = 0;
    if (m_status == Leader) {
      myAssert(false,
               format("[func-sendRequestVote-rf{%d}]  term:{%d} 同一个term当两次领导，error", m_me, m_currentTerm));
    }
    m_status = Leader;
    DPrintf("[func-sendRequestVote rf{%d}] elect success  ,current term:{%d} ,lastLogIndex:{%d}\n", m_me, m_currentTerm,
            getLastLogIndex());
    int lastLogIndex = getLastLogIndex();
    for (int i = 0; i < m_nextIndex.size(); i++) {
        m_nextIndex[i] = lastLogIndex + 1;  //这里设置为下一次需要增加的日志下标，错了也没关系，会回退
        m_matchIndex[i] = 0;                
        //每换一个领导都是从0开始，表示追随者一定和领导者相同的日志部分，由于这里不知道新领导者和追随者日志哪些相同
        //，固然设置为0
    }
    std::thread t(&Raft::doHeartBeat, this);  //马上向其他节点宣告自己就是leader
    t.detach();

    persist();
    return true;
}

void Raft::Start(Op command, int* newLogIndex, int* newLogTerm, bool* isLeader)
{
    std::lock_guard<std::mutex> lg1(m_mtx);
    //只有领导者可以启动
    if (m_status != Leader) {
        DPrintf("[func-Start-rf{%d}]  is not leader");
        *newLogIndex = -1;
        *newLogTerm = -1;
        *isLeader = false;
        return;
    }
    raftRpcProctoc::LogEntry newLogEntry;
    //可能是一个自定义函数，通常用于将某种数据结构（比如 bytes、二进制数据、或特定类）转换为字符串
    newLogEntry.set_command(command.asString());
    newLogEntry.set_logterm(m_currentTerm);
    newLogEntry.set_logindex(getNewCommandIndex());
    m_logs.emplace_back(newLogEntry);
    int lastLogIndex = getLastLogIndex();
    DPrintf("[func-Start-rf{%d}]  lastLogIndex:%d,command:%s\n", m_me, lastLogIndex, &command);

    persist();
    *newLogIndex = newLogEntry.logindex();
    *newLogTerm = newLogEntry.logterm();
    *isLeader = true;
/*
 服务或测试人员想要创建一个 Raft 服务器。所有 Raft 服务器（包括这个）的端口
 都在 peers[] 中。这个
 服务器的端口是 peers[me]。所有服务器的 peers[] 数组
 具有相同的顺序。persister 是此服务器保存其持久状态的地方，并且最初还保存最近保存的状态（如果有）。
 applyCh 是测试人员或服务期望 Raft 发送 ApplyMsg 消息的通道。
 */
}
// 获取新命令应该分配的Index
int Raft::getNewCommandIndex() 
{
    auto lastLogIndex = getLastLogIndex();
    return lastLogIndex + 1;
}

//初始化某一个raft结点：
void Raft::init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me, std::shared_ptr<Persister> persister,
                std::shared_ptr<LockQueue<ApplyMsg>> applyCh) 
{
    m_peers = peers;
    m_persister = persister;
    m_me = me;

    m_mtx.lock();
    //客户端和raft结点进行通信的部分：
    this->applyChan = applyCh;
    m_currentTerm = 0;
    m_status = Follower;
    m_commitIndex = 0;
    m_lastApplied = 0;
    m_logs.clear();
    for (int i = 0; i < m_peers.size(); i++) {
        m_matchIndex.push_back(0);
        m_nextIndex.push_back(0);
    }
    //表示可以选举的意思
    m_votedFor = -1;
    m_lastSnapshotIncludeIndex = 0;
    m_lastSnapshotIncludeTerm = 0;
    m_lastResetElectionTime = now();
    m_lastResetHearBeatTime = now();
    //从之前保存的状态进行初始化,读取之前保存的内容
    readPersist(m_persister->ReadRaftState());
    //这里还需要看一下
    if (m_lastSnapshotIncludeIndex > 0) {
        m_lastApplied = m_lastSnapshotIncludeIndex;
        // rf.commitIndex = rf.lastSnapshotIncludeIndex   todo ：崩溃恢复为何不能读取commitIndex（不懂）
    }
    DPrintf("[Init&ReInit] Sever %d, term %d, lastSnapshotIncludeIndex {%d} , lastSnapshotIncludeTerm {%d}", m_me,
            m_currentTerm, m_lastSnapshotIncludeIndex, m_lastSnapshotIncludeTerm);
    m_mtx.unlock();
    m_ioManager = std::make_unique<monsoon::IOManager>(FIBER_THREAD_NUM, FIBER_USE_CALLER_THREAD);
    //针对选举部分：开启三个协程定时器进行处理：
    //三个函数中leaderHearBeatTicker 、electionTimeOutTicker执行时间是恒定的
    //applierTicker时间受到数据库响应延迟和两次apply之间请求数量的影响，这个随着数据量增多可能不太合理，最好其还是启用一个线程
    m_ioManager->scheduler([this]() -> void { this->leaderHearBeatTicker(); });
    m_ioManager->scheduler([this]() -> void { this->electionTimeOutTicker(); }); ////用于检查是否自身需要发起选举
    std::thread t3(&Raft::applierTicker, this);
    t3.detach();

}

//用于检查是否自身需要发起选举
void Raft::electionTimeOutTicker()
{
    //如果不睡眠，那么对于leader，这个函数会一直空转，浪费cpu。且加入协程之后，空转会导致其他协程无法运行，对于时间敏感的AE，会导致心跳无法正常发送导致异常
    while(true){
        while (m_status == Leader) {
            //休眠微妙级别：和sleep的秒数不是一个单位
            usleep(HeartBeatTimeout);  //定时时间没有严谨设置，因为HeartBeatTimeout比选举超时一般小一个数量级，因此就设置为HeartBeatTimeout了
        }
        //逻辑和心跳定时差不多，getRandomizedElectionTimeout这个函数位于commonn文件夹内，会生成一个大于心跳超时时间的
        //随机数
        std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
        std::chrono::system_clock::time_point wakeTime{};
        {
            m_mtx.lock();
            wakeTime = now();
            suitableSleepTime = getRandomizedElectionTimeout() + m_lastResetElectionTime - wakeTime;
            m_mtx.unlock();
        }
        if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1) {
            // 获取当前时间点
            auto start = std::chrono::steady_clock::now();
            usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());
            // 获取函数运行结束后的时间点
            auto end = std::chrono::steady_clock::now();
            // 计算时间差并输出结果（单位为毫秒）
            std::chrono::duration<double, std::milli> duration = end - start;
            // 使用ANSI控制序列将输出颜色修改为紫色
            std::cout << "\033[1;35m electionTimeOutTicker();函数设置睡眠时间为: "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                    << std::endl;
            std::cout << "\033[1;35m electionTimeOutTicker();函数实际睡眠时间为: " << duration.count() << " 毫秒\033[0m"
                    << std::endl;
        }
        if (std::chrono::duration<double, std::milli>(m_lastResetElectionTime - wakeTime).count() > 0) {
        //说明睡眠的这段时间有重置定时器，那么就没有超时，再次睡眠
            continue;
        }
        doElection();
    }

}




void Raft::leaderHearBeatTicker() 
{
    while(true){
        //  不是leader的话就没有必要进行后续操作，况且还要拿锁，很影响性能，目前是睡眠，后面再优化优化
        while (m_status != Leader) {
            usleep(1000 * HeartBeatTimeout);
        }
        static std::atomic<int32_t> atomicCount = 0; //统计心跳了多少次
        std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
        std::chrono::system_clock::time_point wakeTime{}; //表示当前时间
        //通过milliseconds(HeartBeatTimeout) + m_lastResetHearBeatTime - wakeTime计算是否超时
        //如果大于1的话，那么就说明距离超时时间还长，可以将当前线程让出进行sleep，没必要阻塞，如果段的话就直接不睡眠了
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            wakeTime = now();
            suitableSleepTime = std::chrono::milliseconds(HeartBeatTimeout) + m_lastResetHearBeatTime - wakeTime;
        }
        if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1) {
            std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数设置睡眠时间为: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                << std::endl;
            // 获取当前时间点
            auto start = std::chrono::steady_clock::now();

            usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());
            // std::this_thread::sleep_for(suitableSleepTime);

            // 获取函数运行结束后的时间点
            auto end = std::chrono::steady_clock::now();

            // 计算时间差并输出结果（单位为毫秒）
            std::chrono::duration<double, std::milli> duration = end - start;

            // 使用ANSI控制序列将输出颜色修改为紫色
            std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数实际睡眠时间为: " << duration.count()
                    << " 毫秒\033[0m" << std::endl;
            ++atomicCount;
        }
        //说明有发生心跳
        if (std::chrono::duration<double, std::milli>(m_lastResetHearBeatTime - wakeTime).count() > 0) {
        //睡眠的这段时间有重置定时器，没有超时，再次睡眠
            continue;
        }
        doHeartBeat();
    }
}


std::vector<ApplyMsg> Raft::getApplyLogs()
{
    std::vector<ApplyMsg> applyMsgs;
    myAssert(m_commitIndex <= getLastLogIndex(), format("[func-getApplyLogs-rf{%d}] commitIndex{%d} >getLastLogIndex{%d}",
                                                    m_me, m_commitIndex, getLastLogIndex()));
    while(m_lastApplied < m_commitIndex){
        m_lastApplied++;
        myAssert(m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex() == m_lastApplied,
                 format("rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogIndex{%d} != rf.lastApplied{%d} ",
                        m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex(), m_lastApplied));
        ApplyMsg applyMsg;
        applyMsg.CommandValid = true;
        applyMsg.SnapshotValid = false;
        applyMsg.Command = m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].command();
        applyMsg.CommandIndex = m_lastApplied;
        applyMsgs.emplace_back(applyMsg);
    }
    return applyMsgs;
}

//这个应该是应用到客户端和服务端的管道的定时机制：定时将一些日志进行放入管道，以便客户端可以拿？
void Raft::applierTicker()
{
    while(true){
        m_mtx.lock();
        if (m_status == Leader) {
            DPrintf("[Raft::applierTicker() - raft{%d}]  m_lastApplied{%d}   m_commitIndex{%d}", m_me, m_lastApplied,
                    m_commitIndex);
        }
        auto applyMsgs = getApplyLogs();
        m_mtx.unlock();
        if (!applyMsgs.empty()) {
            DPrintf("[func- Raft::applierTicker()-raft{%d}] 向kvserver報告的applyMsgs長度爲：{%d}", m_me, applyMsgs.size());
        }
        for(auto & message : applyMsgs){
            applyChan->Push(message);
        }
        //进行睡眠
        sleepNMilliseconds(ApplyInterval);
    }

}

//获取当前的结点的状态
void Raft::GetState(int* term, bool* isLeader) {
    m_mtx.lock();
    DEFER {
        // todo 暂时不清楚会不会导致死锁
        m_mtx.unlock();
    };

    // Your code here (2A).
    *term = m_currentTerm;
    *isLeader = (m_status == Leader);
}

void Raft::persist() 
{
    //保存到这里面，获取的是日志的信息，没有获取快照的具体信息
    auto data = persistData();
    //将其保存到状态机中
    m_persister->SaveRaftState(data);
}


//将该结点的所有数据，进行序列化，然后变成一个字符串返回过去
std::string Raft::persistData() {
    BoostPersistRaftNode boostPersistRaftNode;
    boostPersistRaftNode.m_currentTerm = m_currentTerm;
    boostPersistRaftNode.m_votedFor = m_votedFor;
    boostPersistRaftNode.m_lastSnapshotIncludeIndex = m_lastSnapshotIncludeIndex;
    boostPersistRaftNode.m_lastSnapshotIncludeTerm = m_lastSnapshotIncludeTerm;
    for (auto& item : m_logs) {
        boostPersistRaftNode.m_logs.push_back(item.SerializeAsString()); //SerializeAsString() 是一个常见的 Protobuf 方法，将日志条目转换为二进制字符串表示。
    }
    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);
    oa << boostPersistRaftNode;
    return ss.str();
}

//反序列化：并将内容从状态机恢复到结点中
void Raft::readPersist(std::string data) {
    if (data.empty()) {
        return;
    }
    std::stringstream iss(data);
    boost::archive::text_iarchive ia(iss);
    // read class state from archive
    BoostPersistRaftNode boostPersistRaftNode;
    ia >> boostPersistRaftNode;

    m_currentTerm = boostPersistRaftNode.m_currentTerm;
    m_votedFor = boostPersistRaftNode.m_votedFor;
    m_lastSnapshotIncludeIndex = boostPersistRaftNode.m_lastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = boostPersistRaftNode.m_lastSnapshotIncludeTerm;
    m_logs.clear();
    for (auto& item : boostPersistRaftNode.m_logs) {
        raftRpcProctoc::LogEntry logEntry;
        logEntry.ParseFromString(item);
        m_logs.emplace_back(logEntry);
    }
}

//更新领导者的提交记录
void Raft::leaderUpdateCommitIndex() 
{
    m_commitIndex = m_lastSnapshotIncludeIndex;
    for (int index = getLastLogIndex(); index >= m_lastSnapshotIncludeIndex + 1; index--) {
        int sum = 0;
        for (int i = 0; i < m_peers.size(); i++) {
            if (i == m_me) {
                sum += 1;
                continue;
            }
            if (m_matchIndex[i] >= index) {
                sum += 1;
            }
        }
    //只有当前日志被一半以上的结点认可了，才可以,后面的认可了，那么前面的也一定认可，这里是通过从后往前的方式寻找，更快一点
    if (sum >= m_peers.size() / 2 + 1 && getLogTermFromLogIndex(index) == m_currentTerm) {
        m_commitIndex = index;
        break;
    }
  }

}
//判断一个候选者或请求方的日志是否比当前节点的日志更加“新”
bool Raft::UpToDate(int index, int term) {
    int lastIndex = -1;
    int lastTerm = -1;
    getLastLogIndexAndTerm(&lastIndex, &lastTerm);
    return term > lastTerm || (term == lastTerm && index >= lastIndex);
}


void Raft::RequestVote(const raftRpcProctoc::RequestVoteArgs* args, raftRpcProctoc::RequestVoteReply* reply)
{
    std::lock_guard<std::mutex> lg(m_mtx);
    DEFER {
        //应该先持久化，再撤销lock
        persist();
    };
    //可能这个竞选者出现了网络分区，比如这个只有这个竞选者出现了与领导者的通信不畅,但是该接收结点可以与领导者正常
    //固然此时接收结点任期比这个任期要大
    if (args->term() < m_currentTerm) {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Expire);
        reply->set_votegranted(false);
        return;
    }
    if (args->term() > m_currentTerm) {
        m_status = Follower;
        m_currentTerm = args->term();
        m_votedFor = -1;   //-1表示可以进行投票，表示票还没有被用
    }
    myAssert(args->term() == m_currentTerm,
               format("[func--rf{%d}] 前面校验过args.Term==rf.currentTerm，这里却不等", m_me));
    //还需要检查log的term和index是不是匹配的了
    int lastLogTerm = getLastLogTerm();
    //说明日志太旧了,不给这个结点进行投票
    if (!UpToDate(args->lastlogindex(), args->lastlogterm())) {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Voted);
        reply->set_votegranted(false);
        return ;
    }
    m_votedFor = args->candidateid(); //表示投给这个候选者
    m_lastResetElectionTime = now();  //必须要在投出票的时候才重置定时器
    reply->set_term(m_currentTerm);
    reply->set_votestate(Normal);
    reply->set_votegranted(true);

    return;
}

void Raft::Snapshot(int index, std::string snapshot)
{
    std::lock_guard<std::mutex> lg(m_mtx);
    //第一个条件表示之前做的快照包含index下标的快照了，这次不能做了。
    //后者是快照必须要保证一致性，固然是大多数结点都认可的日志，所以必须在提交的范围之内
    if (m_lastSnapshotIncludeIndex >= index || index > m_commitIndex) {
        DPrintf(
            "[func-Snapshot-rf{%d}] rejects replacing log with snapshotIndex %d as current snapshotIndex %d is larger or "
            "smaller ",
            m_me, index, m_lastSnapshotIncludeIndex);
        return;
    }
    auto lastLogIndex = getLastLogIndex();  //为了检查snapshot前后日志是否一样，防止多截取或者少截取日志
    //表示下一次需要快照的位置是index往后的部分你，因为往前部分都已经被快照了
    int newLastSnapshotIncludeIndex = index;
    int newLastSnapshotIncludeTerm = m_logs[getSlicesIndexFromLogIndex(index)].logterm();
    std::vector<raftRpcProctoc::LogEntry> trunckedLogs; //表示剩下的日志，这边忽略了真正快照的代码部分，是逻辑实现：利用这个变量
    for (int i = index + 1; i <= getLastLogIndex(); i++) {
        //注意有=，因为要拿到最后一个日志
        trunckedLogs.push_back(m_logs[getSlicesIndexFromLogIndex(i)]);
    }
    m_lastSnapshotIncludeIndex = newLastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = newLastSnapshotIncludeTerm;
    m_logs = trunckedLogs;
    //感觉这一步没有必要
    m_commitIndex = std::max(m_commitIndex, index);
    m_lastApplied = std::max(m_lastApplied, index);
    //持久化的是快照以后的日志
    m_persister->Save(persistData(), snapshot);

    DPrintf("[SnapShot]Server %d snapshot snapshot index {%d}, term {%d}, loglen {%d}", m_me, index,
              m_lastSnapshotIncludeTerm, m_logs.size());
    myAssert(m_logs.size() + m_lastSnapshotIncludeIndex == lastLogIndex,
               format("len(rf.logs){%d} + rf.lastSnapshotIncludeIndex{%d} != lastLogjInde{%d}", m_logs.size(),
                      m_lastSnapshotIncludeIndex, lastLogIndex));

}
