SCAN_DEPS := ../source.mk
SY108_RECIPE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk
PKG_NAME:=udx-pmsys
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
SY108_SOURCE_PATHS := packages/udx-pmsys
include $(SY108_RECIPE_DIR)../source.mk
include $(INCLUDE_DIR)/package.mk

Build/Prepare = $(call SY108/Prepare,$(PKG_BUILD_DIR))
Build/Compile =

define KernelPackage/udx710-pmsys
  SUBMENU:=Other modules
  TITLE:=SY108 PM_SYS remote processor
  DEPENDS:=@TARGET_sprd
  KCONFIG:=CONFIG_SPRD_UDX710_PMSYS
  FILES:=$(LINUX_DIR)/drivers/remoteproc/udx710-pmsys.ko
endef

define KernelPackage/udx710-pmsys/install
	$(INSTALL_DIR) $(1)/lib/preinit $(1)/lib/firmware/sprd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/packages/udx-pmsys/files/07_sy108-pmsys $(1)/lib/preinit/
	$(LN) /tmp/sy108-firmware/udx710-pmsys.bin $(1)/lib/firmware/sprd/udx710-pmsys.bin
endef
$(eval $(call KernelPackage,udx710-pmsys))
