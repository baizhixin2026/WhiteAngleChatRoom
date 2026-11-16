#ifndef ERRCODES_H
#define ERRCODES_H

#include <QString>

/*
 * 统一错误码 + 面向用户的友好中文文案
 *
 * 规矩：
 *   1. errcode 给程序判断，text 给人看，两者必须成对下发，不允许只有其中一个。
 *   2. 面向用户的文案只允许来自 Err::friendlyText()；
 *      禁止把 errorString() / QSqlError / errno / 文件路径 拼进 text。
 *   3. 技术细节只进服务端日志（logMessage）。
 *   4. 文案标准：说清「发生了什么 + 用户该做什么」，不带编号、不带英文、不带冒号堆砌。
 *
 * 分段：
 *   1xxx 通用参数   2xxx 身份与权限   3xxx 文件与存储   4xxx 传输   5xxx 服务端
 */
namespace Err {

enum Code : int {
    Ok                     = 0,

    // ---- 1xxx 通用 ----
    Unknown                = 1000,
    BadRequest             = 1001,
    InvalidParam           = 1002,
    PayloadTooLarge        = 1003,

    // ---- 2xxx 身份与权限 ----
    NotAuthenticated       = 2000,
    PermissionDenied       = 2001,
    NotFriend              = 2002,
    NotGroupMember         = 2003,
    AccountBanned          = 2004,
    PasswordIncorrect      = 2005,
    PasswordTooWeak        = 2006,
    PasswordSameAsOld      = 2007,

    // ---- 3xxx 文件与存储 ----
    FileNotFound           = 3000,
    FileTooLarge           = 3001,
    FileTypeNotAllowed     = 3002,
    StorageFull            = 3003,
    StorageIoError         = 3004,
    FileCorrupted          = 3005,
    UploadBusy             = 3006,
    UploadExpired          = 3007,
    UploadOffsetMismatch   = 3008,
    FileNotReady           = 3009,
    RangeInvalid           = 3010,
    QuotaExceeded          = 3011,
    FileNameEmpty          = 3012,
    FileNameTooLong        = 3013,
    FileCorruption         = 3014,
    FileDownloadFailed     = 3015,
    DownloadBusy           = 3016,

    // ---- 4xxx 传输 ----
    TooManyTransfers       = 4000,
    TransferNotFound       = 4001,
    TransferAborted        = 4002,
    RateLimited            = 4003,
    TransferCongested      = 4004,

    // ---- 5xxx 服务端 ----
    ServiceUnavailable     = 5000,
    DbError                = 5001,
    VersionMismatch        = 5002
};

// 面向用户的唯一文案来源（两端共用，保证口径一致）
inline QString friendlyText(int code)
{
    switch (code) {
    case Ok:                    return QString();

    case BadRequest:            return QStringLiteral("请求内容有误，请重试");
    case InvalidParam:          return QStringLiteral("操作失败，请重试");
    case PayloadTooLarge:       return QStringLiteral("内容超出大小限制, 最大2G");

    case NotAuthenticated:      return QStringLiteral("登录状态已失效，请重新登录");
    case PermissionDenied:      return QStringLiteral("没有权限访问该文件");
    case NotFriend:             return QStringLiteral("仅好友之间可以发送文件");
    case NotGroupMember:        return QStringLiteral("你已不在该群聊中");
    case AccountBanned:         return QStringLiteral("账号已被封禁, 操作失败");
    case PasswordIncorrect:     return QStringLiteral("旧密码输入错误");
    case PasswordTooWeak:       return QStringLiteral("新密码长度错误， 6-20 位");
    case PasswordSameAsOld:     return QStringLiteral("新密码与旧密码相同");

    case FileNotFound:          return QStringLiteral("文件不存在或已被删除");
    case FileTooLarge:          return QStringLiteral("文件太大，请选择更小的文件");
    case FileTypeNotAllowed:    return QStringLiteral("暂不支持这种类型的文件");
    case StorageFull:           return QStringLiteral("服务器存储空间不足，请稍后再试");
    case StorageIoError:        return QStringLiteral("文件保存失败，请重试");
    case FileCorrupted:         return QStringLiteral("文件已损坏，请重新发送");
    case UploadBusy:            return QStringLiteral("该文件正在上传中，请稍后重试");
    case UploadExpired:         return QStringLiteral("上传已超时，请重新发送");
    case UploadOffsetMismatch:  return QStringLiteral("文件传输中断，请重新发送");
    case FileNotReady:          return QStringLiteral("文件还在处理中，请稍后重试");
    case RangeInvalid:          return QStringLiteral("文件读取出错，文件可能已更新，请重新下载");
    case QuotaExceeded:         return QStringLiteral("你的存储空间已满，请清理后重试");
    case FileNameEmpty:         return QStringLiteral("文件名为空，请填写文件名");
    case FileNameTooLong:       return QStringLiteral("文件名过长，请重命名");
    case FileCorruption:        return QStringLiteral("该文件信息已损坏，无法下载");
    case FileDownloadFailed:    return QStringLiteral("文件下载失败，请重试");
    case DownloadBusy:          return QStringLiteral("该文件正在下载中，请稍后重试");

    case TooManyTransfers:      return QStringLiteral("传输的文件数量，达到限制数量，请等待完成后重试");
    case TransferNotFound:      return QStringLiteral("传输状态异常，请重新发送文件");
    case TransferAborted:       return QStringLiteral("传输已取消");
    case RateLimited:           return QStringLiteral("操作太频繁，请稍后再试");
    case TransferCongested:     return QStringLiteral("文件传输繁忙，请稍后重试，会从断点继续");

    case ServiceUnavailable:    return QStringLiteral("服务暂时不可用，请稍后重试");
    case DbError:               return QStringLiteral("服务暂时不可用，请稍后重试");
    case VersionMismatch:       return QStringLiteral("客户端版本过低，请升级后使用");

    default:                    return QStringLiteral("操作没有完成，请稍后重试");
    }
}

// 是否是「连接已不可用」类错误：这类错误不做重试提示，直接走掉线流程
inline bool isConnectionFatal(int code)
{
    return code == NotAuthenticated || code == VersionMismatch;
}

} // namespace Err

#endif // ERRCODES_H
