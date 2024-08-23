#ifndef RAFTRPC_H
#define RAFTRPC_H

//������װ��һЩ�����stub���������

#include "raftRPC.pb.h"
/// @brief ά����ǰ�ڵ������ĳһ����������rpc����ͨ�ŵĹ���
// ����һ��raft�ڵ���˵���������������Ľڵ㶼Ҫά��һ��rpc���ӣ���MprpcChannel

class RaftRpcUtil
{
private:
    raftRpcProctoc::raftRpc_Stub *stub_; //raftRpcProctocΪ�ļ��Զ���������ռ�

public:
      //�������������ڵ����������,���԰���mit6824�����ã����Ǳ�Ľڵ�����Լ��ĺ���Ͳ����ˣ�Ҫ�̳�protoc�ṩ��service�����
    //������־
    bool AppendEntries(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response);
    //����
    bool InstallSnapshot(raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapshotResponse *response);
    //����ѡ��
    bool RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response);

    //��Ӧ�������ķ���(���ﻹû��ʵ��),������չ��ʱ����ܻ����

    //���캯������ֱ�ӽ�������
    RaftRpcUtil(std::string ip,short port);
    ~RaftRpcUtil();
};

#endif  // RAFTRPC_H