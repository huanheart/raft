#ifndef RAFT_H
#define RAFT_H

#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "ApplyMsg.h"
#include "Persister.h"
#include "boost/any.hpp"
#include "boost/serialization/serialization.hpp"
#include "config.h"
#include "monsoon.h"
#include "raftRpcUtil.h"
#include "util.h"

/// @brief //////////// 网络状态表示  todo：可以在rpc中删除该字段，实际生产中是用不到的.
constexpr int Disconnected =
    0;  // 方便网络分区的时候debug，网络异常的时候为disconnected，只要网络正常就为AppNormal，防止matchIndex[]数组异常减小
constexpr int AppNormal = 1;

///////////////投票状态

constexpr int Killed = 0;
constexpr int Voted = 1;   //本轮已经投过票了
constexpr int Expire = 2;  //投票（消息、竞选者）过期
constexpr int Normal = 3;

class Raft : public raftRpcProctoc::raftRpc{
private:
    std::mutex m_mtx;
    std::vector<std::shared_ptr<RaftRpcUtil> > m_peers;
     std::shared_ptr<Persister> m_persister;
    int m_me;
    int m_currentTerm;
    int m_votedFor;
    std::vector<raftRpcProctoc::LogEntry> m_logs;  //包含了状态机要执行的指令集，以及收到领导时的任期号（也可以用来确认心跳机制的）

    int m_commitIndex;
    int m_lastApplied;  // 已经汇报给状态机（上层应用）的log 的index（不是很懂，回来看）

    std::vector<int>  m_nextIndex;  // 这两个状态的下标1开始，因为通常commitIndex和lastApplied从0开始，应该是一个无效的index，因此下标从1开始
    std::vector<int>  m_matchIndex; //这两个看文档1
    //表示当前身份
    enum Status { 
        Follower, 
        Candidate,
         Leader 
    };
     Status m_status;
    //服务端与服务端之间有着自己的通信方式，这里是客户端与raft的通信方式
    std::shared_ptr<LockQueue<ApplyMsg>> applyChan;  // client从这里取日志（2B），client与raft通信的接口
    
    // 选举超时
    std::chrono::_V2::system_clock::time_point m_lastResetElectionTime; 
    // 心跳超时，用于leader
    std::chrono::_V2::system_clock::time_point m_lastResetHearBeatTime;

    // 2D中用于传入快照点
    // 储存了快照中的最后一个日志的Index和Term
    int m_lastSnapshotIncludeIndex;
    int m_lastSnapshotIncludeTerm;

    // 协程库中自己封装的协程：这边是调度器
    std::unique_ptr<monsoon::IOManager> m_ioManager = nullptr;

public:
    void AppendEntries1(const raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *reply);
    void applierTicker();
    bool CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot);
    void doElection();

    //brief 发起心跳，只有leader才需要发起心跳
    void doHeartBeat();

    // 每隔一段时间检查睡眠时间内有没有重置定时器，没有则说明超时了(原因：如果当前结点没有收到领导者的心跳，即没有被他给重置，那么可能认为领导者失效了
    //所以可能会发起投票选举了
    // 如果有则设置合适睡眠时间：睡眠到重置时间+超时时间（利用时间戳进行重置）
    void electionTimeOutTicker();
    std::vector<ApplyMsg> getApplyLogs();
    int getNewCommandIndex();
    void getPrevLogInfo(int server, int *preIndex, int *preTerm);
    void GetState(int *term, bool *isLeader);
    void InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest *args,
                           raftRpcProctoc::InstallSnapshotResponse *reply);
    void leaderHearBeatTicker();
    void leaderSendSnapShot(int server);
    void leaderUpdateCommitIndex();
    bool matchLog(int logIndex, int logTerm);
    void persist();
    void RequestVote(const raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *reply);
    bool UpToDate(int index, int term);
    int getLastLogIndex();
    int getLastLogTerm();
    void getLastLogIndexAndTerm(int *lastLogIndex, int *lastLogTerm);
    int getLogTermFromLogIndex(int logIndex);
    int GetRaftStateSize();
    int getSlicesIndexFromLogIndex(int logIndex);

    bool sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                        std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum);
    bool sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                             std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply, std::shared_ptr<int> appendNums);
    //这里并没有拿锁就执行这个了，防止拿锁的时间太长，影响性能
    //将其放入到kvserver里面
    void pushMsgToKvServer(ApplyMsg msg);
    void readPersist(std::string data);
    std::string persistData();

    void Start(Op command, int *newLogIndex, int *newLogTerm, bool *isLeader);

    //这个函数的主要目的是将已经包含在快照中的日志条目丢弃，并用新的快照数据替换当前状态。
    //同时，它还会更新快照下标（即index），表示系统已经应用了这个索引之前的所有日志条目。
    void Snapshot(int index, std::string snapshot);
public:
      // 重写基类方法,因为rpc远程调用真正调用的是这个方法
    //序列化，反序列化等操作rpc框架都已经做完了，因此这里只需要获取值然后真正调用本地方法即可。
    //这个方法会优先调用AppendEntries1这个函数
    void AppendEntries(google::protobuf::RpcController *controller, const ::raftRpcProctoc::AppendEntriesArgs *request,
                         ::raftRpcProctoc::AppendEntriesReply *response, ::google::protobuf::Closure *done) override;
    void InstallSnapshot(google::protobuf::RpcController *controller,
                           const ::raftRpcProctoc::InstallSnapshotRequest *request,
                           ::raftRpcProctoc::InstallSnapshotResponse *response, ::google::protobuf::Closure *done) override;
    void RequestVote(google::protobuf::RpcController *controller, const ::raftRpcProctoc::RequestVoteArgs *request,
                       ::raftRpcProctoc::RequestVoteReply *response, ::google::protobuf::Closure *done) override;

public:
    void init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me, std::shared_ptr<Persister> persister,
            std::shared_ptr<LockQueue<ApplyMsg>> applyCh);

private:
    //针对persist部分：有点不懂为什么那么多的成员与raft本身这个类相同
class BoostPersistRaftNode {
    public:
        friend class boost::serialization::access;
        //serialize模板函数，该函数是Boost.Serialization库的一个特性
        //利用内部重载&，做到使其序列化（我们只用调用&就可以直接使得这些被序列化了）
        template <class Archive>
        void serialize(Archive &ar, const unsigned int version) { 
            ar &m_currentTerm;
            ar &m_votedFor;
            ar &m_lastSnapshotIncludeIndex;
            ar &m_lastSnapshotIncludeTerm;
            ar &m_logs;
        }
        int m_currentTerm;
        int m_votedFor;
        int m_lastSnapshotIncludeIndex;
        int m_lastSnapshotIncludeTerm;
        std::vector<std::string> m_logs;
        std::unordered_map<std::string, int> umap;
   public:
  };

};


#endif  // RAFT_H