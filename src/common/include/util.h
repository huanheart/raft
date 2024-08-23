#ifndef UTIL_H
#define UTIL_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/access.hpp>
#include <condition_variable>  // pthread_condition_t
#include <functional>
#include <iostream>
#include <mutex>  // pthread_mutex_t
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#include "config.h"

//利用了RAII的思想，以及go的思想，在末尾的时候自动释放一些内容，防止内存泄漏,设置为不能拷贝的类
//这里有一个函数细节，详情请看文档2中的util部分
template < class F>
class DeferClass {
public:
    DeferClass(F&& f) : m_func(std::forward<F>(f)) {}
    DeferClass(const F& f) : m_func(f) {}
    ~DeferClass() { m_func(); }

    DeferClass(const DeferClass& e) = delete;
    DeferClass& operator=(const DeferClass& e) = delete;

private:
    F m_func;
};


#define _CONCAT(a, b) a##b
#define _MAKE_DEFER_(line) DeferClass _CONCAT(defer_placeholder, line) = [&]()

#undef DEFER
#define DEFER _MAKE_DEFER_(__LINE__)

void DPrintf(const char* format, ...);

void myAssert(bool condition, std::string message = "Assertion failed!");

template <typename... Args>
std::string format(const char * format_str,Args... args)
{
    std::stringstream ss;
    int _[]={ ( (ss << args),0 )... };
    (void)_;
    return ss.str();
}


std::chrono::_V2::system_clock::time_point now();

std::chrono::milliseconds getRandomizedElectionTimeout();
void sleepNMilliseconds(int N);


//下面是异步写日志的日志队列:
template <typename T>
class LockQueue 
{
public:
    //由于多个worker线程都会写日志，固然我们直接将锁封装在队列的内部，实现一个有锁队列
    void Push(const T& data){
        std::lock_guard<std::mutex> lock(m_mutex); //RAII思想;
        m_queue.push(data);
        m_condvariable.notify_one(); //为什么唤醒一个？因为只能一个线程进行读取，而不是所有线程进行读取
    }

    T Pop(){
        std::unique_lock<std::mutex> lock(m_mutex);
        while(m_queue.empty()){
            //日志队列空的时候，那么线程直接进入wait状态，直到条件变量唤醒
            m_condvariable.wait(lock);
        }
        T data=m_queue.front();
        m_queue.pop();
        return data;
    }
    //添加一个超时参数，默认为50ms(这个是如果指定时间内还没有pop，那么我们就不pop了)，防止一直进行一个等待
    bool timeOutPop(int timeout,T* ResData)  
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        // 获取当前时间点，并计算出超时时刻
        auto now = std::chrono::system_clock::now();
        auto timeout_time = now + std::chrono::milliseconds(timeout);

        // 在超时之前，不断检查队列是否为空
        while (m_queue.empty()) {
        // 如果已经超时了，就返回一个空对象
            if (m_condvariable.wait_until(lock, timeout_time) == std::cv_status::timeout) {
                return false;
            } else {
                continue;
            }
        }

        T data = m_queue.front();
        m_queue.pop();
        *ResData = data;
        return true;
    }

private:
    std::queue<T> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_condvariable;
};



//这个Op是kv传递给raft的command
class Op{
public:
    //这里所有的字段必须以大写开头，否则rpc将会被中断：
    std::string Operation; //选项：判断是get put append哪个方法
    std::string Key;
    std::string Value;
    std::string ClientId; //客户端号码
    int RequestId;         //客户端号码请求的Request的序列号，为了保证线性一致性
    //重复命令不能被应用两次，只能用于 PUT 和 APPEND

public:
    //后期可以换成更高级的序列化方法，比如protobuf
    //为了协调raftRPC中的command只设置成了string,这个的限制就是正常字符中不能包含|(可能是|这个作为分隔符？但是不理解这里不是protobuf吗？)
    //他这里并没有序列化成protobuf，似乎直接利用text_oarchive这些东西直接序列化成易于读取的文本格式
    std::string asString() const {
        std::stringstream ss;
        boost::archive::text_oarchive oa(ss);
        //将一个类实例写入到里面
        oa<<*this;

        return ss.str();
    }
    bool parseFromString(std::string str){
        std::stringstream iss(str);
        boost::archive::text_iarchive ia(iss);
        //读取
        ia >> *this;
        return true;  //如果解析失败如何处理？得看一下boost库源码
    }

public:
    //设置为友元的原因：boost库需要访问到我封装的类的私有成员，固然需要进行友元操作
    friend std::ostream& operator<<(std::ostream& os, const Op& obj) {
        os << "[MyClass:Operation{" + obj.Operation + "},Key{" + obj.Key + "},Value{" + obj.Value + "},ClientId{" +
              obj.ClientId + "},RequestId{" + std::to_string(obj.RequestId) + "}";  // 在这里实现自定义的输出格式
        return os;
    }

private:
    friend class boost::serialization::access ;
    //设置为友元的原因：boost库需要访问到我封装的类的私有成员，固然需要进行友元操作
    template <class Archive>
    void serialize(Archive& ar, const unsigned int version) { //用于序列化的：为什么使用&，具体看分布式存储文档2,她并不一定是位与，可能进行重载了
        ar& Operation;
        ar& Key;
        ar& Value;
        ar& ClientId;
        ar& RequestId;
    }


};

const std::string OK = "OK";
const std::string ErrNoKey = "ErrNoKey";
const std::string ErrWrongLeader = "ErrWrongLeader";

//获取可用端口
bool isReleasePort(unsigned short usPort);

bool getReleasePort(short& port);


#endif