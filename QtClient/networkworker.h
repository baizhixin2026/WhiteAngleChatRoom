#ifndef NETWORKWORKER_H
#define NETWORKWORKER_H

#include <QObject>
#include <QTcpSocket>
#include <QQueue>
#include <QJsonObject>
#include "protocoltypes.h"

class NetworkWorker : public QObject
{
    Q_OBJECT
public:
    explicit NetworkWorker(QObject *parent = nullptr);
     ~NetworkWorker();

public slots:
    void init();
    void connectToServer(const QString &ip,quint16 port);
    void disconnectFromServer();
    void cleanup();
    void flushPending();    // 刷新待发送队列
    void sendJson(const QJsonObject &obj);
    void sendFileChunk(const FileChunk &c);
    void sendFileMessage(const FileMessage &m);

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &error);

    void chatMessageReceived(const ChatMessage &msg);
    void chatAckReceived(const ChatAck &msg);
    void historyMsgReceived(const HistoryPage &msg);
    void recentChatReceived(const RecentChatResult &msg);
    void sysMessageReceived(const SysMessage &msg);
    void nicknameChangeResult(const NicknameChangeResult &msg);
    void passwordChangeResult(const PasswordChangeResult &msg);

    void fileMessageReceived(const FileMessage &msg);
    void fileChunkReceived(const FileChunk &chunk);

private:
    void clearSessionBuffers();
    void abortSocket();

    // 高优先级：聊天/回执/心跳。可以与文件数据乱序，绝对不会被大文件饿死。
    void sendUrgentFrame(const QByteArray &frame);

    // 有序队列：文件块 + 文件控制面（begin/end/abort）。三者必须严格保持先后。
    void sendOrderedFrame(const QByteArray &frame);

private:
    QTcpSocket *m_socket = nullptr;
    QByteArray m_buffer;

    QQueue<QByteArray> m_pendingUrgent;    // 高优先级队列
    QQueue<QByteArray> m_pendingOrdered;   // 文件数据面专线
};

#endif // NETWORKWORKER_H
