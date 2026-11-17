-- Personal sell and keep lists edited from the PocketVendor addon
CREATE TABLE IF NOT EXISTS `mod_pocket_vendor_list` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'Character GUID',
  `item_id` INT UNSIGNED NOT NULL,
  `list` TINYINT UNSIGNED NOT NULL COMMENT '0 = sell, 1 = keep',
  PRIMARY KEY (`guid`, `item_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
