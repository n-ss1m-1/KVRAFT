#include<unistd.h>
#include<fcntl.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#include<errno.h>
#include<string.h>
#include<iostream>


#include "tcp_server.h"
#include "../common/command.h"


//?可改进为非阻塞
int CreateFd()
{
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd==-1) 
    {
        perror("CreateFd");
        exit(EXIT_FAILURE);         //▲自然退出->析构->关闭fd
    }
    return fd;
}


TcpServer::TcpServer(std::shared_ptr<KVStore> store,const std::string& ip,int port)
    :started_(false),
    ip_(ip),
    port_(port),
    listenFd_(CreateFd()),
    connFd_(-1),
    store_(store)
{
    //开启端口复用(避免TIMEWAIT等待)+禁用Nagle算法
    int opt=1;
    if(setsockopt(listenFd_,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt))==-1)
    {
        perror("setsockopt SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }
    if(setsockopt(listenFd_,IPPROTO_TCP,TCP_NODELAY,&opt,sizeof(opt))==-1)
    {
        perror("setsockopt TCP_NODELAY");
        exit(EXIT_FAILURE);
    }
}

TcpServer::~TcpServer()
{
    started_=false;

    //在析构处自然关闭fd
    close(listenFd_);
    close(connFd_);
}

void TcpServer::Start()
{
    started_=true;

    //初始化服务器地址
    struct sockaddr_in servAddr,cliAddr;
    socklen_t cliLen;

    memset(&servAddr,0,sizeof(servAddr));
    servAddr.sin_family=AF_INET;
    servAddr.sin_port=htons(port_);
    servAddr.sin_addr.s_addr=htonl(INADDR_ANY);

    //绑定服务器地址
    if(bind(listenFd_,(struct sockaddr*)&servAddr,sizeof(servAddr))==-1)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    //listen
    if(listen(listenFd_,128)==-1)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    std::cout<<"TcpServer:listening..."<<std::endl;

    //循环：accept
    while(started_)
    {
        connFd_=accept(listenFd_,(struct sockaddr*)&cliAddr,&cliLen);
        if(connFd_==-1)
        {
            perror("accept");
            exit(EXIT_FAILURE);
        }
        HandleClient();
    }
}

void TcpServer::Stop()
{
    started_=false;
}

void TcpServer::HandleClient()
{
    std::cout<<"Connect success"<<std::endl;
    char buf[4096];
    std::string buffer;
    while(true)
    {
        //read
        ssize_t len=read(connFd_,buf,sizeof(buf));      //!sizeof->容量 strlen->实际长度
        //错误处理
        if(len==0) 
        {
            //▲客户端关闭
            std::cout<<"Connection close\n";
            return;
        }
        else if(len==-1)
        {
            if(errno==EINTR) continue;
            else 
            {
                perror("read");
                return;                                  //!只退出HandleClient 而不exit退出程序->其他客户端仍能正常连接
            }
        }

        //len>0 -> 处理
        buffer.append(buf,len);

        std::cout<<"receive: "<<buffer<<std::endl;
        
        size_t pos;
        while((pos=buffer.find('\n'))!=std::string::npos)   //以换行符`\n`作为分隔
        {
            //截取一行
            std::string line=buffer.substr(0,pos);          //!截取0~pos: 不含'\n'
            buffer.erase(0,pos+1);                          //!删除0~pos+1: 跳到'\n'后一位      !必须先erase再判断是否为空行->否则直接换行会导致死循环
            if(line.empty()) 
            {
                std::cout<<"test line empty\n";
                continue;                  //空指令 直接跳到下一轮
            }
            

            //解析指令
            auto command=ParseCommand(line);
            if(!command.has_value())
            {
                std::cout<<"Invalid command"<<std::endl;
                continue;
            }
            
            //执行指令
            std::string reply;          //传出参数
            store_->Execute(command.value(),reply);

            std::cout<<"reply: "<<reply<<std::endl;
        
            //循环：write(避免一次写不完)
            size_t written=0;
            size_t remaining=reply.size();                              //!   size()返回size_t类型   read、write返回ssize_t类型
            while(remaining>0)
            {
                ssize_t len=write(connFd_,reply.c_str()+written,remaining);         //!每次写之后都偏移指针
                if(len==-1)
                {
                    if(errno==EINTR) continue;
                    else
                    {
                        perror("write");
                        return;                  //!只退出HandleClient 而不exit退出程序->其他客户端仍能正常连接
                    }
                }
                written+=len;
                remaining-=len;
            }
        }
    }
}
