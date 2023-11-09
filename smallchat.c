#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/select.h>

#define MAX_CLIENTS 1000            // 最大连接数
#define SERVER_PORT 8970            // 服务器端口号

// 客户端
struct client {
    int fd;                  // 文件描述符（套接字描述符）
    char *nick;             // 昵称字符串
};

// 聊天室全局数据结构
struct chatState {
    int serversock;           // 文件描述符（套接字描述符）
    int numclients;           // 目前客户端个数
    int maxclient;            // 最大连接数
    struct client *clients[MAX_CLIENTS];          // 客户端数组                               
};

struct chatState *Chat;         // 全局变量

// 创建一个监听套接字，开始监听指定的端口。这个监听套接字负责接受客户端的连接请求。
int createTCPServer(int port) {
    int server_fd, yes = 1;
    struct sockaddr_in sa;

    // 创建套接字文件描述符
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) return -1;

    // 将套接字绑定到端口8080
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&sa,0,sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    // 将套接字绑定到指定的IP和端口, 监听传入的连接
    if (bind(server_fd,(struct sockaddr*)&sa,sizeof(sa)) == -1 || listen(server_fd, 511) == -1) {
        close(server_fd);
        return -1;
    }

    return server_fd;
}

// 将指定的套接字（socket）设置为非阻塞模式，并开启 TCP 的无延迟（no delay）选项。
int socketSetNonBlockNoDelay(int fd) {
    int flags, yes = 1;

    if ((flags = fcntl(fd, F_GETFL)) == -1) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return 0;
}

// 服务器端会为每个连接请求创建一个新的套接字来与对应的客户端进行通信。
// 这些套接字都是通过调用 accept()函数从监听套接字中接受连接而产生的。
// 每个新创建的套接字都具有自己的文件描述符，用于与相应的客户端进行通信。
int acceptClient(int server_socket) {
    int client_fd;

    while(1) {
        struct sockaddr_in sa;
        socklen_t slen = sizeof(sa);
        client_fd = accept(server_socket,(struct sockaddr*)&sa,&slen);
        if (client_fd == -1) {
            if (errno == EINTR)
                continue; 
            else
                return -1;
        }
        break;
    }
    return client_fd;
}

// 对 malloc封装
void *chatMalloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        perror("Out of memory");
        exit(1);
    }
    return ptr;
}

// 创建一个连接的客户端
struct client *createClient(int fd) {
    char nick[32]; 
    
    // 初始化昵称
    int nicklen = snprintf(nick,sizeof(nick),"user:%d",fd);

    // 开空间
    struct client *c = chatMalloc(sizeof(*c));

    socketSetNonBlockNoDelay(fd); 

    c->fd = fd;
    c->nick = chatMalloc(nicklen+1);

    // 复制昵称
    memcpy(c->nick,nick,nicklen);

    assert(Chat->clients[c->fd] == NULL); 
    Chat->clients[c->fd] = c;
    if (c->fd > Chat->maxclient) Chat->maxclient = c->fd;
    Chat->numclients++;
    return c;
}

// 释放内存
void freeClient(struct client *c) {
    free(c->nick);

    // 关闭套接字连接
    close(c->fd);

    Chat->clients[c->fd] = NULL;
    Chat->numclients--;
    if (Chat->maxclient == c->fd) {
        int j;
        for (j = Chat->maxclient-1; j >= 0; j--) {
            if (Chat->clients[j] != NULL) Chat->maxclient = j;
            break;
        }
        if (j == -1) Chat->maxclient = -1;
    }
    free(c);
}

// 聊天室初始化
void initChat(void) {
    Chat = chatMalloc(sizeof(*Chat));
    memset(Chat,0,sizeof(*Chat));
    Chat->maxclient = -1;
    Chat->numclients = 0;

    // 创建一个监听套接字
    Chat->serversock = createTCPServer(SERVER_PORT);
    if (Chat->serversock == -1) {
        perror("Creating listening socket");
        exit(1);
    }
}

// 向所有客户端广播，除去发送消息的人
void sendMsgToAllClientsBut(int excluded, char *s, size_t len) {
    for (int j = 0; j <= Chat->maxclient; j++) {
        if (Chat->clients[j] == NULL || Chat->clients[j]->fd == excluded){
            continue;
        } 
        // 数据写入发送缓冲区
        // 操作系统会负责处理数据的发送和接收，你可以继续执行其他任务。
        write(Chat->clients[j]->fd,s,len);
    }
}

// 主函数
int main(void) {
    // 初始化
    initChat();

    // 监听
    while(1) {
        fd_set readfds;     // 创建一个文件描述符集合（通常是fd_set类型的数据结构），用于存储待检查的文件描述符。
        struct timeval tv;
        int retval;

        FD_ZERO(&readfds);
        FD_SET(Chat->serversock, &readfds);     // 在文件描述符集合上设置监听套接字的监听事件

        for (int j = 0; j <= Chat->maxclient; j++) {
            if (Chat->clients[j]) FD_SET(j, &readfds);
        }

        tv.tv_sec = 1; 
        tv.tv_usec = 0;

        int maxfd = Chat->maxclient;
        if (maxfd < Chat->serversock) maxfd = Chat->serversock;

        // 在网络编程中，可以使用select()函数来监视多个文件描述符（包括套接字描述符）的状态，
        // 以便实现同时监听多个事件的能力。select()函数是一种多路复用的机制，
        // 可以用于等待一个或多个文件描述符准备就绪，然后进行相应的操作。
        // 类似于一种筛选，把符合的保留，不符合的剔除，这里是筛选 监听套接字和现有客户端套接字是否有数据可读
        retval = select(maxfd+1, &readfds, NULL, NULL, &tv);
        if (retval == -1) {
            perror("select() error");
            exit(1);
        } else if (retval) {
            // 可以通过检查读集合来确定哪些文件描述符准备就绪。
            // 对于监听套接字，表示有新的连接请求。
            if (FD_ISSET(Chat->serversock, &readfds)) {
                int fd = acceptClient(Chat->serversock);
                struct client *c = createClient(fd);
                
                char *welcome_msg =
                    "Welcome to Simple Chat! "
                    "Use /nick <nick> to set your nick.\n";
                
                write(c->fd,welcome_msg,strlen(welcome_msg));
                printf("Connected client fd=%d\n", fd);
            }
            
            char readbuf[256];      // 接受缓冲
            for (int j = 0; j <= Chat->maxclient; j++) {
                if (Chat->clients[j] == NULL) continue;
                // 对于现有客户端套接字，表示有数据可以读取。
                if (FD_ISSET(j, &readfds)) {
                    // 将接受缓冲区的数据存入我们设置的接受缓冲
                    int nread = read(j,readbuf,sizeof(readbuf)-1);

                    if (nread <= 0) {
                    
                        printf("Disconnected client fd=%d, nick=%s\n",
                            j, Chat->clients[j]->nick);
                        freeClient(Chat->clients[j]);
                    } else {
                        // 复制一份 client
                        struct client *c = Chat->clients[j];
                        readbuf[nread] = 0;     // 字符串读取时检测到有一个字符为空，就视为一个完整的字符串

                        // 检测到开头的字符为 '/'时，进入设置昵称分支
                        if (readbuf[0] == '/') {
                            // 剔除制表符
                            char *p;
                            p = strchr(readbuf,'\r'); if (p) *p = 0;
                            p = strchr(readbuf,'\n'); if (p) *p = 0;
                            
                            char *arg = strchr(readbuf,' ');
                            if (arg) {
                                *arg = 0; 
                                arg++; 
                            }

                            if (!strcmp(readbuf,"/nick") && arg) {
                                free(c->nick); // 释放掉原先的默认昵称
                                int nicklen = strlen(arg);
                                c->nick = chatMalloc(nicklen+1);
                                memcpy(c->nick,arg,nicklen+1);
                            } else {
                                char *errmsg = "Unsupported command\n";
                                write(c->fd,errmsg,strlen(errmsg));
                            }
                        } else { // 若开头不是 '/'，则进入发送消息分支
                            char msg[256];
                            int msglen = snprintf(msg, sizeof(msg), "%s> %s", c->nick, readbuf);
                            if (msglen >= (int)sizeof(msg)){
                                msglen = sizeof(msg)-1;
                            }

                            printf("%s",msg);

                            sendMsgToAllClientsBut(j,msg,msglen);
                        }
                    }
                }
            }
        }
    }
    return 0;
}
