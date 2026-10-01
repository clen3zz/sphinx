-- 面向 MySQL 8.4 / InnoDB；启动服务前由具备权限的操作员执行一次。
-- id 是商品主键，version 用于并发更新检查；价格以“分”为单位。
CREATE TABLE IF NOT EXISTS products (
    id BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    name VARCHAR(128) NOT NULL,
    price_cents BIGINT UNSIGNED NOT NULL,
    version BIGINT UNSIGNED NOT NULL DEFAULT 1,
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
        ON UPDATE CURRENT_TIMESTAMP(6),
    CONSTRAINT chk_products_price CHECK (price_cents <= 1000000000000),
    CONSTRAINT chk_products_version CHECK (version > 0),
    CONSTRAINT chk_products_id CHECK (id > 0),
    CONSTRAINT chk_products_name_bytes CHECK (OCTET_LENGTH(name) BETWEEN 1 AND 128)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
