CREATE TABLE `user` (
    `vid`      BIGINT UNSIGNED NOT NULL,
    `username` VARCHAR(64)     NOT NULL,
    `password` VARCHAR(255)    NOT NULL,
    PRIMARY KEY (`vid`),
    UNIQUE KEY `uk_username` (`username`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
