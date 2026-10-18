SCAN_DEPS := ../source.mk
SY108_RECIPE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
include $(TOPDIR)/rules.mk
PKG_NAME:=luci-theme-aurora
PKG_VERSION:=1.4.0
PKG_RELEASE:=1
PKG_LICENSE:=Apache-2.0
SY108_SOURCE_PATHS := packages/luci-theme-aurora
include $(SY108_RECIPE_DIR)../source.mk
include $(INCLUDE_DIR)/package.mk

Build/Prepare = $(call SY108/Prepare,$(PKG_BUILD_DIR))

define Package/luci-theme-aurora
  SECTION:=luci
  CATEGORY:=LuCI
  TITLE:=luci-theme-aurora for LuCI
  DEPENDS:=+luci-base +rpcd-mod-ucode +ucode-mod-fs +ucode-mod-uci +uclient-fetch +jsonfilter
endef

define Build/Compile
endef

define Package/luci-theme-aurora/install
	$(CP) $(PKG_BUILD_DIR)/packages/luci-theme-aurora/root/. $(1)/
endef

$(eval $(call BuildPackage,luci-theme-aurora))
