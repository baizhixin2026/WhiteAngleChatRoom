#ifndef STORAGEREMOVEKEY_H
#define STORAGEREMOVEKEY_H

#include <qstring.h>


/*
 * 存储对象的唯一键："<biz>:<sha256>"
 *
 * 为什么必须是 (biz, sha256) 而不是 sha256：
 *   finalPath 把 biz 编进了目录（<root>/<biz>/xx/yy/<sha256>），
 *   同一份字节用不同 biz 上传会落成两个物理文件，只按 sha 会张冠李戴。
 *
 * 所有跨模块比较的地方都必须用它生成，不要各写一遍字符串拼接
 * （DB 侧产生、存储侧比对，格式必须逐字节一致）。
 */
inline QString storageObjectKey(quint8 biz, const QString &sha256)
{
    return QString::number(biz) + QLatin1Char(':') + sha256;
}

#endif // STORAGEREMOVEKEY_H
