#ifndef APPLYMSG_H
#define APPLYMSG_H

#include<string>


//这个类封装了一些具体信息，比如说快照索引，命令日志下标啊什么的
class ApplyMsg
{
public:
    bool CommandValid;
    std::string Command;
    int CommandIndex;
    bool SnapshotValid;
    std::string Snapshot;
    int SnapshotTerm;
    int SnapshotIndex;

public:
    ApplyMsg()
        : CommandValid(false),
          Command(),
          CommandIndex(-1),
          SnapshotValid(false),
          SnapshotTerm(-1),
          SnapshotIndex(-1){

        };         


};

#endif