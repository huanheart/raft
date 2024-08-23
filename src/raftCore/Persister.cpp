#include "Persister.h"
#include "util.h"



void Persister::Save(const std::string raftstate, const std::string snapshot) 
{
    std::lock_guard<std::mutex> lg(m_mtx);
    clearRaftStateAndSnapshot();
    // 将raftstate和snapshot写入本地文件
    m_raftStateOutStream << raftstate;
    m_snapshotOutStream << snapshot;
    //这里应该加入m_raftStateSize+=size()吧？
}

std::string Persister::ReadSnapshot() 
{
    std::lock_guard<std::mutex> lg(m_mtx);
    if (m_snapshotOutStream.is_open()) {
      m_snapshotOutStream.close();
    }
    //这里延迟调用的意义：因为多个线程中，可能有多个读写正在发生，假设现在抢到了锁，但是其他读写
    //中途进行到一半，那么前面是被这个流打开的，所以当我们读取的时候可能就出现这个情况了，固然我们将其先进行关闭
    //然后此时可以通过另外一个流进行读取当前文件的状态，后面读取完毕，再进行一个文件的关闭即可,锁还给他，并将文件重新打开
    //后面那个没进行完操作的就可以继续进行操作了，很细节的处理,
    DEFER {
      m_snapshotOutStream.open(m_snapshotFileName);  //默认是追加
    };
    std::fstream ifs(m_snapshotFileName, std::ios_base::in);
    if (!ifs.good()) {       //思考：为什么这里不用调用close方法？ 解答：内部调用了(采用RAII的方式)
      return "";
    }
    std::string snapshot;
    ifs >> snapshot;
    //尽管采用RAII的方式，但是显示调用仍然更直观更有意义
    ifs.close();
    return snapshot;
}


void Persister::SaveRaftState(const std::string &data) 
{
    std::lock_guard<std::mutex> lg(m_mtx);
    clearRaftState();
    m_raftStateOutStream << data;
    m_raftStateSize += data.size();
}


long long Persister::RaftStateSize() 
{
    std::lock_guard<std::mutex> lg(m_mtx);
    return m_raftStateSize;
}


//这个函数读取文件，然后将读取到的内容赋值给snapshot并进行返回
std::string Persister::ReadRaftState() 
{
    std::lock_guard<std::mutex> lg(m_mtx);
    std::fstream ifs(m_raftStateFileName, std::ios_base::in); //std::ios_base::in以读的方式打开这个文件
    if (!ifs.good()) { //查看流的状态是否良好，即是否遇到错误
      return "";
    }
    std::string snapshot;
    ifs >> snapshot;
    ifs.close();
    return snapshot;
}


Persister::Persister(const int me)
    : m_raftStateFileName("raftstatePersist" + std::to_string(me) + ".txt"),
      m_snapshotFileName("snapshotPersist" + std::to_string(me) + ".txt"),
      m_raftStateSize(0) 
{
    bool fileOpenFlag = true;
    std::fstream file(m_raftStateFileName, std::ios::out | std::ios::trunc);
    if (file.is_open()) {
      file.close();
    } else {
      fileOpenFlag = false;
    }
    file = std::fstream(m_snapshotFileName, std::ios::out | std::ios::trunc);
    if (file.is_open()) {
      file.close();
    } else {
      fileOpenFlag = false;
    }
    if (!fileOpenFlag) {
      DPrintf("[func-Persister::Persister] file open error");
    }
    //上面这边主要检查这个文件是否能被正常打开关闭这些，便于在排查的时候，可以查看文件打开是否出现问题
    //下面绑定流：方便后续可以用，ofstream是输出写文件的类，方便后续写文件用
    m_raftStateOutStream.open(m_raftStateFileName);
    m_snapshotOutStream.open(m_snapshotFileName);
}


Persister::~Persister()
{
    if (m_raftStateOutStream.is_open()) {
      m_raftStateOutStream.close();
    }
    if (m_snapshotOutStream.is_open()) {
      m_snapshotOutStream.close();
    }

}


void Persister::clearRaftState() {
    m_raftStateSize = 0;
    // 关闭文件流
    if (m_raftStateOutStream.is_open()) {
      m_raftStateOutStream.close();
    }
    // 重新打开文件流并清空文件内容(相当于进行一次刷新)
    //可能保留旧的数据没什么意义,而且防止文件膨胀，具有较多的文件
    m_raftStateOutStream.open(m_raftStateFileName, std::ios::out | std::ios::trunc); //重新打开的时候，会以清空之前的所有内容的方式打开
}


void Persister::clearSnapshot()
{
    if(m_snapshotOutStream.is_open()){
        m_snapshotOutStream.close();
    }
    //为新的写操作做好准备
    m_snapshotOutStream.open(m_snapshotFileName,std::ios::out | std::ios::trunc );
    //这里关闭又打开的操作：为下一个快照文件进行准备，参数std::ios::out的意思是，以追加的形式去创建文件，后面的参数表示如果
    //文件本身有内容，那么就选择清空
    
}


void Persister::clearRaftStateAndSnapshot() {
    clearRaftState();
    clearSnapshot();
}