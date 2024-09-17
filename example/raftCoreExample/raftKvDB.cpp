//
// Created by swx on 23-12-28.
//
#include <iostream>
#include "raft.h"
// #include "kvServer.h"
#include <kvServer.h>
#include <unistd.h>
#include <iostream>
#include <random>

void ShowArgsHelp();

int main(int argc, char **argv) {
  //////////////////////////////////读取命令参数：节点数量、写入raft节点节点信息到哪个文件
  if (argc < 2) {
    ShowArgsHelp();
    exit(EXIT_FAILURE);
  }
  int c = 0;
  int nodeNum = 0;
  std::string configFileName;
  std::random_device rd;
  std::mt19937 gen(rd());  //都是用于生成随机数的东西
  std::uniform_int_distribution<> dis(10000, 29999);
  unsigned short startPort = dis(gen);
  //startPort这个是随机生成的端口号，范围在dis的两个参数那里
  //两个选项分别是raft个数以及文件名
  while ((c = getopt(argc, argv, "n:f:")) != -1) {
    switch (c) {
      case 'n':
        nodeNum = atoi(optarg);
        break;
      case 'f':
        configFileName = optarg;
        break;
      default:
        ShowArgsHelp();
        exit(EXIT_FAILURE);
    }
  }
  //打开这个文件，第一个close以及创建文件感觉没什么意义
  std::ofstream file(configFileName, std::ios::out | std::ios::app);
  file.close();
  //这里是直接trunc将之前的文件所对应的内容进行了一个清空
  file = std::ofstream(configFileName, std::ios::out | std::ios::trunc);
  if (file.is_open()) {
    file.close();
    std::cout << configFileName << " 已清空" << std::endl;
  } else {
    std::cout << "无法打开 " << configFileName << std::endl;
    exit(EXIT_FAILURE);
  }
  //开启服务到对应连续的端口上
  for (int i = 0; i < nodeNum; i++) {
    short port = startPort + static_cast<short>(i);
    std::cout << "start to create raftkv node:" << i << "    port:" << port << " pid:" << getpid() << std::endl;
    pid_t pid = fork();  // 创建新进程,我知道了，上面那个是父进程，getpid，固然我们追踪他是得到了父进程的情况
    //所以我们应该获取当前的pid
    if (pid == 0) {
      // 如果是子进程
      // 子进程的代码
      //在子进程里面创建一个KVServer，去模拟远程其他raft的机器
      auto kvServer = new KvServer(i, 500, configFileName, port);
      pause();  // 子进程进入等待状态，不会执行 return 语句
    } else if (pid > 0) {
      // 如果是父进程
      // 父进程的代码
      sleep(1);
    } else {
      // 如果创建进程失败
      std::cerr << "Failed to create child process." << std::endl;
      exit(EXIT_FAILURE);
    }
  }
  //如果有对应的信号发送过来，此时pause会接收到，否则将会阻塞在这里
  pause();
  return 0;
}

void ShowArgsHelp() { std::cout << "format: command -n <nodeNum> -f <configFileName>" << std::endl; }
