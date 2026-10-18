SCAN_DEPS := ../source.mk
SY108_RECIPE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
include $(TOPDIR)/rules.mk
PKG_NAME:=sy108-image
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_HOST_ONLY:=1
SY108_SOURCE_PATHS := packages/sy108-image tools/image/capsule.py tools/lib/boot_layout.py tools/lib/memory_layout.py tools/lib/newc.py
include $(SY108_RECIPE_DIR)../source.mk
HOST_BUILD_DIR:=$(BUILD_DIR_HOST)/$(PKG_NAME)-$(PKG_VERSION)
include $(INCLUDE_DIR)/package.mk

Build/Prepare = $(call SY108/Prepare,$(PKG_BUILD_DIR))
include $(INCLUDE_DIR)/host-build.mk

Host/Prepare = $(call SY108/Prepare,$(HOST_BUILD_DIR))

define Package/sy108-image
  SECTION:=devel
  CATEGORY:=Development
  TITLE:=SY108 boot capsule host tools
  BUILDONLY:=1
endef

define Host/Compile
endef

define Host/Install
	rm -rf $(STAGING_DIR_HOST)/share/sy108-image
	$(INSTALL_DIR) $(STAGING_DIR_HOST)/share/sy108-image/tools/image
	$(INSTALL_DIR) $(STAGING_DIR_HOST)/share/sy108-image/tools/lib
	$(INSTALL_DATA) $(HOST_BUILD_DIR)/tools/image/capsule.py $(STAGING_DIR_HOST)/share/sy108-image/tools/image/
	$(INSTALL_DATA) $(HOST_BUILD_DIR)/tools/lib/boot_layout.py $(HOST_BUILD_DIR)/tools/lib/memory_layout.py $(HOST_BUILD_DIR)/tools/lib/newc.py $(STAGING_DIR_HOST)/share/sy108-image/tools/lib/
endef

define Host/Clean
	rm -rf $(STAGING_DIR_HOST)/share/sy108-image
endef

$(eval $(call BuildPackage,sy108-image))
$(eval $(call HostBuild))
