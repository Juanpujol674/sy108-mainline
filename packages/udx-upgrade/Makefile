SCAN_DEPS := ../source.mk
SY108_RECIPE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
include $(TOPDIR)/rules.mk
PKG_NAME:=udx-upgrade
PKG_VERSION:=4.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
SY108_SOURCE_PATHS := packages/udx-upgrade components/boot-format components/system
include $(SY108_RECIPE_DIR)../source.mk
include $(INCLUDE_DIR)/package.mk

Build/Prepare = $(call SY108/Prepare,$(PKG_BUILD_DIR))

define Package/udx-upgrade
 SECTION:=admin
 CATEGORY:=Administration
 TITLE:=SY108 platform upgrade support
 DEPENDS:=+udx-runtime +fwtool +libubus +libubox +libuci +libfdt +ubi-utils +fstools
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 -Os -Wall -Wextra \
	 -I$(PKG_BUILD_DIR)/components/boot-format -I$(PKG_BUILD_DIR)/components/system \
	 $(PKG_BUILD_DIR)/packages/udx-upgrade/src/boot-state.c $(PKG_BUILD_DIR)/packages/udx-upgrade/src/management-ready.c $(PKG_BUILD_DIR)/components/boot-format/misc-state.c \
	 $(TARGET_LDFLAGS) -lubus -lubox -luci -o $(PKG_BUILD_DIR)/udx-boot-state
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 -Os -Wall -Wextra \
	 -I$(PKG_BUILD_DIR)/components/boot-format \
	 $(PKG_BUILD_DIR)/packages/udx-upgrade/src/image-check.c $(PKG_BUILD_DIR)/components/boot-format/capsule.c \
	 $(PKG_BUILD_DIR)/components/boot-format/misc-state.c $(TARGET_LDFLAGS) -lfdt -o $(PKG_BUILD_DIR)/udx-image-check
endef

define Package/udx-upgrade/install
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/udx-boot-state $(1)/usr/libexec/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/udx-image-check $(1)/usr/libexec/
	$(CP) $(PKG_BUILD_DIR)/packages/udx-upgrade/files/. $(1)/
endef
$(eval $(call BuildPackage,udx-upgrade))
