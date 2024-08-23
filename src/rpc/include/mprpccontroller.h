#pragma once
#include <google/protobuf/service.h>
#include<string>

class MprpcController : public google::protobuf::RpcController
{
public:
    MprpcController();
    void Reset();
    bool Failed() const;
    std::string ErrorText() const;
    void SetFailed(const std::string & reason);

    // 目前未实现具体的功能,具体需要实现的话得进行重写，否则不能编译成功
    void StartCancel();
    bool IsCanceled() const;
    void NotifyOnCancel(google::protobuf::Closure* callback);

private:
    bool m_failed;   //rpc执行过程中的状态
    std::string m_errText;    //rpc执行过程中的错误信息
};