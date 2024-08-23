#ifndef SKIP_LIST_ON_RAFT_PERSISTER_H
#define SKIP_LIST_ON_RAFT_PERSISTER_H
#include <fstream>
#include <mutex>

//有关持久化
class Persister
{
private:
    std::mutex m_mtx;
    std::string m_raftState;
    std::string m_snapshot;

    //raftState的文件名
    const std::string m_raftStateFileName;
    //快照文件名
    const std::string m_snapshotFileName;
    //保存raftState的输出流
    std::ofstream m_raftStateOutStream;
    //保存快照的输出流
    std::ofstream m_snapshotOutStream;
   //保存raftStateSize的大小
   //避免每次都读取文件来获取具体的大小(这里还不是很懂)
    long long m_raftStateSize;

public:
    //一些公共接口
    void Save(std::string m_raftstate,std::string m_snapshot);
    std::string ReadSnapshot();
    void SaveRaftState(const std::string& data);
    long long RaftStateSize();
    std::string ReadRaftState();
    //explicit禁止出现隐式转换：比如说一个int就可以赋值给一个对象这种情况
    //一般用于构造函数，拷贝函数等地方
    explicit Persister(int me);
    ~Persister();
private:
    //都用于为下一个快照文件等内容做准备，固然析构函数的时候不能用这些函数
    void clearRaftState();
    void clearSnapshot();
    void clearRaftStateAndSnapshot();
};

#endif