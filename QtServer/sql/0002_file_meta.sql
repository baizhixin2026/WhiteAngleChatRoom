-- ============================================================================
-- 文件传输元数据（协议 v4）
-- 一期只标记不物理删除，所以没有级联删除，只有 status 变更。
-- ============================================================================

CREATE TABLE IF NOT EXISTS files (
  id          BIGINT AUTO_INCREMENT PRIMARY KEY,
  file_id     VARCHAR(36)  NOT NULL UNIQUE,          -- 对外 UUID（无花括号）
  sha256      CHAR(64)     NOT NULL,                 -- 内容寻址键，小写十六进制
  size        BIGINT       NOT NULL DEFAULT 0,
  mime        VARCHAR(128) NOT NULL DEFAULT '',
  file_name   VARCHAR(255) NOT NULL DEFAULT '',      -- 显示名，服务端已清洗
  owner_uid   BIGINT       NOT NULL,
  biz         TINYINT      NOT NULL,
  status      TINYINT      NOT NULL DEFAULT 0,       -- 0上传中 1可用 2已删除
  ref_count   INT          NOT NULL DEFAULT 0,       -- 为后续引用计数预留，一期不参与判断
  created_at  BIGINT       NOT NULL,
  ready_at    BIGINT       NOT NULL DEFAULT 0,
  KEY idx_owner (owner_uid, status),
  KEY idx_sha   (sha256, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 读权限的唯一来源，不从消息表反推；群聊上线只需加 grantee_type=2 分支
CREATE TABLE IF NOT EXISTS file_acl (
  file_id       VARCHAR(36) NOT NULL,
  grantee_type  TINYINT     NOT NULL,                -- 0=public 1=uid 2=group
  grantee_id    BIGINT      NOT NULL DEFAULT 0,
  granted_by    BIGINT      NOT NULL DEFAULT 0,
  created_at    BIGINT      NOT NULL,
  PRIMARY KEY (file_id, grantee_type, grantee_id),
  KEY idx_grantee (grantee_type, grantee_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 引用位置（生命周期用，不参与权限判断）
CREATE TABLE IF NOT EXISTS file_refs (
  file_id     VARCHAR(36)  NOT NULL,
  owner_uid   BIGINT       NOT NULL,
  biz         TINYINT      NOT NULL,
  biz_id      VARCHAR(64)  NOT NULL DEFAULT '',
  created_at  BIGINT       NOT NULL,
  PRIMARY KEY (file_id, owner_uid, biz, biz_id),
  KEY idx_owner (owner_uid, biz)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
