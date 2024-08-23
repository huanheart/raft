#include "include/util.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>


void myAssert(bool condition,std::string message)  //自己实现一个小断言(判断)
{
    if(!condition){
        std::cerr << "Error: "<< message <<std::endl; 
        std::exit(EXIT_FAILURE); //非正常退出
    }
}


//高精度时钟：now函数本身线程安全的
std::chrono::_V2::system_clock::time_point now() 
{ 
    return std::chrono::high_resolution_clock::now(); 
}




//获取随机选举超时
std::chrono::milliseconds getRandomizedElectionTimeout()
{
    //一个随机数种子
    std::random_device rd; 
    std::mt19937 rng(rd());
    //std::mt19937 是一种伪随机数生成器，基于梅森旋转算法（Mersenne Twister）。它以 std::random_device 生成的随机数作为种子
    //确保每次程序运行时的随机数序列不同。std::mt19937 提供了一个很好的平衡，既有高效的生成速度，又有较长的周期
    
    //均匀分布，生成一个min到max的整数，且生成每一个数字的概率相同
    std::uniform_int_distribution<int> dist(minRandomizedElectionTime, maxRandomizedElectionTime); //minRandomizedElectionTime定义在config.h中
    //将生成的随机数转换成ms进行一个返回
     return std::chrono::milliseconds(dist(rng));
}

void sleepNMilliseconds(int N)  //进行一个睡眠
{ 
    std::this_thread::sleep_for(std::chrono::milliseconds(N)); 
};

//查看哪个端口可以使用
bool getReleasePort(short &port) 
{
    short num=0;
    while(!isReleasePort(port)&&num<30 ){
        ++port;
        ++num;
    }
    if(num>=30){
        port = -1;
        return false;
    }
    return true;
}


//判断当前端口是否正在使用
bool isReleasePort(unsigned short usPort)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(usPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ret = ::bind(s, (sockaddr *)&addr, sizeof(addr));

    if(ret!=0){ //说明这个端口正在被占用，反正绑定失败了
        close(s);
        return false;
    }
    close(s);
    return true;
}

// 获取当前的日期，然后取日志信息，写入相应的日志文件当中 a+
void DPrintf(const char * format, ...)
{
    if(Debug){
        time_t now=time(nullptr); //获取当前时间
        tm *nowtm =localtime(&now); //转化成本地时间
        va_list args;     //va_list 用于访问可变函数中的可变参数
        va_start(args, format);
        //输出对应时间
        std::printf("[%d-%d-%d-%d-%d-%d] ", nowtm->tm_year + 1900, nowtm->tm_mon + 1, nowtm->tm_mday, nowtm->tm_hour,
                nowtm->tm_min, nowtm->tm_sec);
        //用于输出可变参数的函数
        std::vprintf(format, args);
        std::printf("\n");
        //结束可变参数的访问
        va_end(args);
    }
}
