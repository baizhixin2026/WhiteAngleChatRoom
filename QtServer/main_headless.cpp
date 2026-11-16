#include <QCoreApplication>
#include <QSocketNotifier>
#include <QDebug>
#include <QtGlobal>
#include <csignal>
#include <unistd.h>

#include "connectionmanager.h"

namespace {
    int g_sigFd[2] = {-1, -1}; // 匿名管道，g_sigFd[0] 是读端，g_sigFd[1] 是写端

    // 信号处理函数里只能调 async-signal-safe 的东西：一个 write 就够
    void unixSignalHandler(int) {
        const char c = 1;
        ::write(g_sigFd[1], &c, 1);
    }
}

/* 运行在云服务器
 * make distclean 2>/dev/null || true
 * qmake CONFIG+=headless
 * make -j1 2>&1 | tee /tmp/build.log
*/
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv); // 非 GUI 程序, 只需要 QtCore, 没有窗口，不需要平台插件

    auto &conn = ConnectionManager::instance();

    // 所有日志直出 stdout，systemd 的 journalctl 直接抓
    QObject::connect(&conn, &ConnectionManager::logMessage, [](const QString &m) { qInfo().noquote() << m; });
    QObject::connect(&conn, &ConnectionManager::serverListenOk, [](QString m) { qInfo().noquote() << "[开始监听]" << m; });
    QObject::connect(&conn, &ConnectionManager::serverListenFail,
                        [](QString m) { qCritical().noquote() << "[监听失败]" << m; });

    // 从环境变量 CHAT_PORT 读取监听端口，做合法性校验，不合法或没设置 就用默认端口 8899
    bool portOk = false;
    const int portInt = qEnvironmentVariableIntValue("CHAT_PORT", &portOk);
    const quint16 port = (portOk && portInt > 0 && portInt <= 65535) ? quint16(portInt) : quint16(8899);

    // 数据库就绪后再开始监听，避免客户端连上来但 DB 还没好
    QObject::connect(&conn, &ConnectionManager::dbReady, [&conn, port](bool ready) {
        if (!ready) {
            qCritical() << "数据库未就绪，服务端不监听";
            return;
        }
        qInfo().noquote() << "数据库就绪，开始监听端口: " << port;
        conn.startServerListen(port);
    });

    // SIGTERM / SIGINT → 走 Qt 事件循环优雅退出（保留 .part、线程 quit）
    if (::pipe(g_sigFd) == 0) {  // 创建匿名管道: 返回 0 成功，-1 失败
        // QSocketNotifier: Qt对epoll/select/kqueue 的封装：把一个裸 fd 注册进 Qt 事件循环，当该 fd 出现指定事件（这里是Read可读）时发出activated信号。
        // 管道 fd 可读是能唤醒 epoll 的标准事件源
        auto *sn = new QSocketNotifier(g_sigFd[0], QSocketNotifier::Read, &app);
        QObject::connect(sn, &QSocketNotifier::activated, [&app]() {
            char c = 0;
            ::read(g_sigFd[0], &c, 1);
            qInfo() << "收到退出信号，正在关闭...";
            ConnectionManager::shutdown();
            app.quit();
        });
        ::signal(SIGTERM, unixSignalHandler);
        ::signal(SIGINT,  unixSignalHandler);
    }

    qInfo().noquote() << "chat-server (headless) 服务端启动";
    return app.exec();
}
