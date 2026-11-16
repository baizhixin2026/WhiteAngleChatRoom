#include "networkworker.h"
#include "protocolcodec.h"
#include <QHostAddress>

// 上行水位：只管"socket 里能排多少"。上行带宽高，给大一点，磁盘读取偶尔慢一下也不掉吞吐。
static constexpr qint64 kUploadSendWaterBytes = 256 * 1024;

NetworkWorker::NetworkWorker(QObject *parent)
    : QObject{parent}
{

}

void NetworkWorker::init()
{
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &NetworkWorker::connected);
    connect(m_socket, &QTcpSocket::disconnected, this, &NetworkWorker::disconnected);
    connect(m_socket, &QTcpSocket::bytesWritten, this, &NetworkWorker::flushPending);

#if QT_VERSION >= QT_VERSION_CHECK(5,15,0)  // Qt 5.15 及以上 新语法
    connect(m_socket, &QTcpSocket::errorOccurred, this, [=](QAbstractSocket::SocketError) {
        emit errorOccurred(m_socket->errorString());
    });
#else   // Qt 5.14 及以下 旧语法
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, [=](QAbstractSocket::SocketError socketError) {
        Q_UNUSED(socketError);
        emit errorOccurred(m_socket->errorString());
    });
#endif

    // 接收数据
    connect(m_socket, &QTcpSocket::readyRead, this, [this]() {
        const QByteArray chunk = m_socket->readAll();
        if(!chunk.isEmpty()) {
            m_buffer.append(chunk);
        }

        for (;;) {
            quint8 ch;
            quint8 version;
            QByteArray payload;
            const FrameStatus status =ProtocolCodec::takeFrame(m_buffer, &ch, &version, &payload);
            if (status == FrameStatus::NeedMore)
                break;
            if (status == FrameStatus::BadChannel || status == FrameStatus::FrameTooLarge) {
                m_socket->abort();
                return;
            }

            // version 服务端升级了
            if (version != Proto::kVersion) {
                emit errorOccurred(QStringLiteral("客户端版本低，请升级客户端"));
                m_socket->abort();
                return;
            }
            if(ch == Proto::kChJson) { // 消息通道
                QJsonObject obj;
                if(!ProtocolCodec::toJsonObject(payload, &obj))
                    continue;

                if (obj.value("type").toString() == "version_mismatch") {
                    emit errorOccurred(QStringLiteral("客户端版本低，请升级客户端"));
                    m_socket->abort();
                    return;
                }

                ChatMessage m;
                HistoryPage h;
                RecentChatResult r;
                SysMessage s;
                ChatAck a;
                NicknameChangeResult n;
                PasswordChangeResult p;
                FileMessage f;
                if (ProtocolCodec::objectToStruct(obj, &m)) {
                    emit chatMessageReceived(m);
                    continue;
                }
                else if (ProtocolCodec::objectToStruct(obj, &a)) {
                    emit chatAckReceived(a);
                    continue;
                }
                else if (ProtocolCodec::objectToStruct(obj, &h)) {
                    emit historyMsgReceived(h);
                    continue;
                }
                else if (ProtocolCodec::objectToStruct(obj, &r)) {
                    emit recentChatReceived(r);
                    continue;
                }
                else if (ProtocolCodec::objectToStruct(obj, &s)) {
                    emit sysMessageReceived(s);
                    continue;
                }
                else if (ProtocolCodec::objectToStruct(obj, &n)) {
                    emit nicknameChangeResult(n);
                    continue;
                }
                else if (ProtocolCodec::objectToStruct(obj, &p)) {
                    emit passwordChangeResult(p);
                    continue;
                }
                else if (ProtocolCodec::objectToStruct(obj, &f)) {
                    emit fileMessageReceived(f);
                    continue;
                }

                qInfo() << "QTcpSocket::readyRead unknown type, ignored: "
                        << obj.value("type").toString();
            }
            else if(ch == Proto::kChFile) {
                FileChunk c;
                if (ProtocolCodec::toFileChunk(payload, &c)) {
                    emit fileChunkReceived(c);
                }
            }
        }
    });
}

void NetworkWorker::clearSessionBuffers()
{
    m_buffer.clear();
    m_pendingUrgent.clear();
    m_pendingOrdered.clear();
}

void NetworkWorker::abortSocket()
{
    if (!m_socket || m_socket->state() == QAbstractSocket::UnconnectedState)
        return;
    // abort()：立即关闭并丢弃内核/Qt 内部缓冲，且会发出 disconnected()
    m_socket->abort();
}

NetworkWorker::~NetworkWorker()
{
    clearSessionBuffers();
    abortSocket();
}

void NetworkWorker::connectToServer(const QString &ip,quint16 port)
{
    if(!m_socket)
        return;
    clearSessionBuffers();
    abortSocket();
    m_socket->connectToHost(QHostAddress(ip), port);
}

void NetworkWorker::disconnectFromServer()
{
    if(!m_socket)
        return;
    clearSessionBuffers();
    abortSocket();
}

void NetworkWorker::cleanup()
{
    clearSessionBuffers();
    abortSocket();
}

// 0x01 通道：文件控制面（begin / end / abort）
// 必须和文件块走同一条有序队列。upload_end 一旦抢到还没写出去的块前面，服务端会拿不完整的数据算 sha256，直接判"文件已损坏"。
void NetworkWorker::sendFileMessage(const FileMessage &m)
{
    if (m.type.isEmpty())
        return;
    sendOrderedFrame(ProtocolCodec::encodeJson(ProtocolCodec::toJson(m)));
}

// 0x01 通道 数据入队 ：聊天 / 回执 / 心跳 , 抢在文件数据前面，不会被大文件堵住
void NetworkWorker::sendJson(const QJsonObject &obj)
{
    sendUrgentFrame(ProtocolCodec::encodeJson(obj));
}

// 0x02 通道 文件块数据入队
void NetworkWorker::sendFileChunk(const FileChunk &c)
{
    sendOrderedFrame(ProtocolCodec::encodeFileChunk(c));
}

void NetworkWorker::sendUrgentFrame(const QByteArray &frame)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;     // 已断线：直接丢，别往关闭的 socket 写
    if (frame.isEmpty())
        return;
    m_pendingUrgent.enqueue(frame);
    flushPending();
}

void NetworkWorker::sendOrderedFrame(const QByteArray &frame)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;
    if (frame.isEmpty())
        return;
    m_pendingOrdered.enqueue(frame);
    flushPending();
}


void NetworkWorker::flushPending()
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    // 高优先级：永远先写，不看水位
    while (!m_pendingUrgent.isEmpty())
        m_socket->write(m_pendingUrgent.dequeue());

    // 有序队列：低于低水位才补。文件块和它的 end 在同一条队列里，顺序天然正确。
    while (!m_pendingOrdered.isEmpty() && m_socket->bytesToWrite() < kUploadSendWaterBytes)
        m_socket->write(m_pendingOrdered.dequeue());
}
