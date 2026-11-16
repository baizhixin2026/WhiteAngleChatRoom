#ifndef FILETRANSFERTYPES_H
#define FILETRANSFERTYPES_H

#include "protocoltypes.h"

#include <QMetaType>
#include <QString>

/*
 * 跨线程纪律：只传值拷贝，禁止跨线程持有指针（与服务端 TransferCtx 同一规矩）
 * GUI 线程(FileTransferManager)：创建、写 transferId / fileId / 状态
 * 网络线程(NetworkWorker)       ：只读
 * 磁盘线程(FileIoWorker)        ：只读，另外自己持有句柄和读游标
 */

struct FileTransferTask {
    quint32 taskId     = 0;      // 客户端本地自增，不等于服务端 transferId，用于三线程对账
    quint32 transferId = 0;      // 服务端在 begin_ok 里分配；0 = 还没有
    quint8  direction  = Proto::TransferUpload;
    quint8  biz        = Proto::BizAvatar;
    QString localPath;           // 上传：源文件绝对路径；下载：落盘临时文件
    QString fileName;            // 显示名（服务端会再清洗一次）
    QString mime;                // 文件的内容格式标识: 纯文本/图片/视频
    QString sha256;              // 上传：本地预计算；下载：begin_ok 回填
    QString fileId;              // 上传：end_ok 后才有；下载：begin 时就有
    qint64  totalSize  = 0;
    qint64  sentBytes  = 0;      // 上传：已交给网络线程的字节数
    qint64  startOffset   = 0;   // 下载：本次请求的起点
    qint64  receivedBytes = 0;   // 下载：已交给磁盘线程的字节数
};

// 一次传输当前处在哪一步。同一个任务同时只有一个值。
enum class TransferState : quint8 {
    Idle = 0,
    Hashing,         // 上传独有：磁盘线程正在算 sha256（还没发 begin，不占服务端槽位）
    Queued,          // 已创建，等闸门放行
    BeginSent,       // 已发 *_begin，等 begin_ok
    OpeningSource,   // 上传：等磁盘线程打开源文件
    Streaming,       // 数据面进行中
    EndSent,         // 上传：已发 end，等服务端 end_ok
    Finalizing,      // 下载：字节已收满，等本地 rename 落盘
    Done,
    Failed,
    Canceled
};

// 传输优先级: 用户操作优先于后台预取
enum class TransferPriority : quint8 {
    User = 0,       // 优先级高：用户主动发起，User(用户操作)
    Background = 1  // 优先级低：Background(头像等后台预取)
};

Q_DECLARE_METATYPE(FileTransferTask)
Q_DECLARE_METATYPE(TransferState)

#endif // FILETRANSFERTYPES_H
