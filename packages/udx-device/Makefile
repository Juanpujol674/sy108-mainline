SCAN_DEPS := ../source.mk
SY108_RECIPE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
include $(TOPDIR)/rules.mk
PKG_NAME:=udx-device
PKG_VERSION:=3.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
SY108_SOURCE_PATHS := packages/udx-device packages/udx-cellular/src/native
include $(SY108_RECIPE_DIR)../source.mk
include $(INCLUDE_DIR)/package.mk

Build/Prepare = $(call SY108/Prepare,$(PKG_BUILD_DIR))

define Package/udx-device
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=SY108 native device RPC
  DEPENDS:=+libjson-c +rpcd +udx-cellular +udx-usb
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 -Wno-misleading-indentation \
	 -I$(PKG_BUILD_DIR)/packages/udx-cellular/src/native \
	 $(PKG_BUILD_DIR)/packages/udx-device/src/device.c $(PKG_BUILD_DIR)/packages/udx-cellular/src/native/common.c \
	 $(TARGET_LDFLAGS) -ljson-c -o $(PKG_BUILD_DIR)/udx-device-rpc
endef

define Package/udx-device/install
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/udx-device-rpc $(1)/usr/libexec/rpcd/udx
endef
$(eval $(call BuildPackage,udx-device))
