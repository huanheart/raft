#ifndef SKIP_LIST_ON_RAFT_KVSERVER_H
#define SKIP_LIST_ON_RAFT_KVSERVER_H

// #include <boost/any.hpp>
// #include <boost/archive/binary_iarchive.hpp>
// #include <boost/archive/binary_oarchive.hpp>
// #include <boost/archive/text_iarchive.hpp>
// #include <boost/archive/text_oarchive.hpp>
// #include <boost/foreach.hpp>
// #include <boost/serialization/export.hpp>
// #include <boost/serialization/serialization.hpp>
// #include <boost/serialization/unordered_map.hpp>
// #include <boost/serialization/vector.hpp>
// #include <iostream>
// #include <mutex>
// #include <unordered_map>
// #include "../../raftRpcPro/include/kvServerRPC.pb.h"
// #include "raft.h"
// #include "../../skipList/include/skipList.h"


// #include <sys/stat.h>
// #include <sys/types.h>
#include <netinet/in.h>
#include <boost/any.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/foreach.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>
#include <iostream>
#include <mutex>
#include <unordered_map>



#include "kvServerRPC.pb.h"
#include "raft.h"
#include "skipList.h"

class KvServer : raftKVRpcProctoc::kvServerRpc
{

private:
    std::mutex m_mtx;
    int m_me;
    std::shared_ptr<Raft> m_raftNode;
    std::shared_ptr<LockQueue<ApplyMsg> > applyChan;  // kvServer和raft节点的通信管道
    int m_maxRaftState;                               // 如果日志增长到这么大，则进行快照
    //序列化后的数据
    std::string m_serializedKVData; 
    SkipList<std::string, std::string> m_skipList;
    //m_kvDB是之前没有写跳表的时候用的哈希表
    std::unordered_map<std::string, std::string> m_kvDB;
    std::unordered_map<int, LockQueue<Op> *> waitApplyCh;

    std::unordered_map<std::string, int> m_lastRequestId;  // clientid -> requestID  //一个kV服务器可能连接多个client

    int m_lastSnapShotRaftLogIndex;
public:
    KvServer() = delete;

    KvServer(int me, int maxraftstate, std::string nodeInforFileName, short port);

    void StartKVServer();

    void DprintfKVDB();

    void ExecuteAppendOpOnKVDB(Op op);
    //主要是通过key获取到当前的value
    void ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist);
    //插入或更改
    void ExecutePutOpOnKVDB(Op op);

    void Get(const raftKVRpcProctoc::GetArgs *args,raftKVRpcProctoc::GetReply *reply);
    void GetCommandFromRaft(ApplyMsg message);
    //是否重复请求
    bool ifRequestDuplicate(std::string ClientId, int RequestId);

      // clerk 使用RPC远程调用
    void PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply);

    ///一直等待raft传来的applyCh
    void ReadRaftApplyCommandLoop();

    void ReadSnapShotToInstall(std::string snapshot);

    bool SendMessageToWaitChan(const Op &op, int raftIndex);

     // 检查是否需要制作快照，需要的话就向raft之下制作快照
    void IfNeedToSendSnapShotCommand(int raftIndex, int proportion);
    //处理来自 kv.rf.applyCh 的 SnapShot
    void GetSnapShotFromRaft(ApplyMsg message);
    std::string MakeSnapShot();
public:  // for rpc
    void PutAppend(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::PutAppendArgs *request,
                 ::raftKVRpcProctoc::PutAppendReply *response, ::google::protobuf::Closure *done) override;

    void Get(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::GetArgs *request,
           ::raftKVRpcProctoc::GetReply *response, ::google::protobuf::Closure *done) override;

private:
    friend class boost::serialization::access;

    template <class Archive>
    void serialize(Archive &ar, const unsigned int version)  //其他需要序列话和反序列化的字段
    {
        ar &m_serializedKVData;

        // ar & m_kvDB;
        ar &m_lastRequestId;
    }
    //m_serializedKVData这里看似没有被序列化到，但是实际上oa << *this;不光光序列化了*this,还序列化了m_serializedKVData以及m_lastRequestId
    //查看serialize这个函数
    std::string getSnapshotData() 
    {
        m_serializedKVData = m_skipList.dump_file();
        std::stringstream ss;
        boost::archive::text_oarchive oa(ss);
        oa << *this;
        m_serializedKVData.clear();
        return ss.str();
    }
    //进行解析以及存储
    void parseFromString(const std::string &str) 
    {
        std::stringstream ss(str);
        boost::archive::text_iarchive ia(ss);
        ia >> *this;
        m_skipList.load_file(m_serializedKVData);
        m_serializedKVData.clear();
    }


};

#endif