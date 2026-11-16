#ifndef SHARDROUTE_H
#define SHARDROUTE_H

#include <QHash>
#include <QString>

/*
 * 分片路由 —— 网络线程与 FileService 必须用同一套规则，否则块会投错分片。
 *
 *   写（上传）按 sha256：同一物理文件永远落在同一分片， .part 不会跨线程并发写，续传（新 transferId）也回到同一分片。
 *   读（下载）按 transferId：热门文件的多路并发读分摊到各分片，保证负载均匀, 避免单分片热点。
 *
 * 分片数 N 一经验证不可运行期修改（定稿约定：改配置需重启）。
 */

namespace ShardRoute {
    // 分片路由: 同一个 sha256 永远算出同一个分片号, 同一个文件的所有块 投到同一个线程, 断点续传必然回到同一个分片
    inline int writeShard(const QString &sha256, int shardCount) {
        if(shardCount <= 1)
            return 0;
        return int(qHash(sha256) % uint(shardCount));
    }

    // 负载均匀
    inline int readShard(quint32 transferId, int shardCount) {
        if(shardCount <= 1)
            return 0;
        return int(qHash(transferId) % uint(shardCount));
    }
}


#endif // SHARDROUTE_H
