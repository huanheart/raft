#include "include/mprpcconfig.h"

#include<iostream>
#include<string>

//去掉字符串前后的空格
void MprpcConfig::Trim(std::string &src_buf)
{
    //查询第一个不属于传参中的字符集合的字符出现的位置
    int index=src_buf.find_first_not_of(' ');
    //说明存在不属于字符集合中的元素，那么index就是其下标    
    if(index !=-1){
        src_buf=src_buf.substr(index,src_buf.size()-index);
    }
    //去掉字符串后面多余的空格
    index=src_buf.find_last_not_of(' ');
    if(index!=-1){
        src_buf=src_buf.substr(0,index+1);
    }

}

std::string MprpcConfig::Load(const std::string & key)
{
    if(m_configMap.count(key) )
        return m_configMap[key];
    return "";
}

//负责加载配置文件
void MprpcConfig::LoadConfigFile(const char * config_file)
{
//    std::cout<<"go in"<<std::endl;
    FILE * pf=fopen(config_file,"r");
    if(pf==nullptr){
        std::cout<<config_file<<" is not exist! "<<std::endl;
        exit(EXIT_FAILURE); //表示非标准退出
    }
    while(!feof(pf) ) //判断文件是否到达末尾
    {
        char buf[512]={0};
        fgets(buf,512,pf); //读取一行的内容，如果缓冲区的数据要小于一行，那么下次会继续读取同一行的内容，直到遇见换行符
        //去掉前后部分多余的空格
        std::string read_buf(buf);
        Trim(read_buf);

            // 判断#的注释
        if (read_buf[0] == '#' || read_buf.empty()) {
            continue;
        }

        // 解析配置项
        int idx = read_buf.find('=');
        if (idx == -1) {
            // 配置项不合法
            continue;
        }
        std::string key;
        std::string value;
        key=read_buf.substr(0,idx);
        Trim(key);

        int endidx = read_buf.find('\n', idx); //找最后一个字符的位置，因为其为换行符，固然也需要舍弃
        value = read_buf.substr(idx + 1, endidx - idx - 1);
        Trim(value);
        m_configMap.insert({key, value});

    }
    fclose(pf);
}