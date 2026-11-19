readme.md

Qt 版本：5.15.3 
Qt5 即时通讯客户端 + 服务端（C/S），支持文字消息、图片、附件、好友、头像、在线状态。
文件传输定为：上传/下载模型（对象存储） 
通信协议定义在 `shared/protocol/` 目录下，主要包含 `protocolcodec` 和 `protocoltypes`，客户端与服务端通过该协议进行数据交互。

数据库：MySQL 8.0+（依赖 8.0.14+ 的行值比较语法）
开发环境：Ubuntu 22.04 / x86_64，Qt 5.15.3，g++，CONFIG += c++17

模块
账号:注册（服务端生成 8 位账号）、登录、改密、单账号单会话 
好友:按账号搜索、申请/同意/拒绝、删除、在线状态、好友头像变更推送 
聊天:私聊文字、图片、附件、历史消息分页、最近会话、未读、离线消息补推 
文件:上传/下载、断点续传、内容寻址去重、ACL 授权、进度与速率 
资料:昵称修改、头像上传与缓存（内存 + 磁盘）
运维:存储对象回收巡检、磁盘对账、数据留存清理 

多线程
客户端：主线程 + 2 个工作线程(1 网络线程 + 1 文件IO线程)
服务端: 主线程 + 4 个工作线程(1 网络线程 + 1 数据库线程 + 2 存储线程)

数据留存
文件保留 60 天（头像 biz=0 不参与，避免客户端头像失效， users.avatar_file_id 悬空）
会话消息保留 90 天（含未送达的离线消息）
每天执行一次，分批 500 行，批间让出事件循环，不阻塞登录与落库
环境变量可覆盖：CHAT_FILE_RETENTION_DAYS / CHAT_MESSAGE_RETENTION_DAYS
注意：61–90 天的历史消息附件已被清理，点开提示"文件不存在"

服务端编译方式：
1.运行在本地
make distclean
qmake
make -j4
mkdir -p /home/leo/chatFiles
CHAT_DB_PASSWORD=2222 CHAT_STORAGE=/home/leo/chatFiles ./QtServer

2.运行在云服务器
make distclean 2>/dev/null || true
qmake CONFIG+=headless
make -j1 2>&1 | tee /tmp/build.log

