SCAN_DEPS := ../source.mk
SY108_RECIPE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
include $(TOPDIR)/rules.mk

PKG_NAME:=udx-usb
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKGARCH:=all

SY108_SOURCE_PATHS := packages/udx-usb
include $(SY108_RECIPE_DIR)../source.mk
include $(INCLUDE_DIR)/package.mk

Build/Prepare = $(call SY108/Prepare,$(PKG_BUILD_DIR))

define Package/udx-usb
  SECTION:=net
  CATEGORY:=Network
  TITLE:=UDX fixed USB management gadget
  DEPENDS:=+netifd +procd +uci +ubus +jsonfilter +flock +dnsmasq +firewall4 +busybox +agetty +@BUSYBOX_CUSTOM +@BUSYBOX_CONFIG_LOGIN +@BUSYBOX_CONFIG_TASKSET
endef

define Package/udx-usb/description
  NCM hardware and software management modes with an optional
  password-authenticated ACM console. Requires the platform kernel's configfs,
  libcomposite, NCM, PAM U3 and ACM support; does not control USB power/role.
endef

define Package/udx-usb/conffiles
/etc/config/udx-usb
endef

define Build/Compile
endef

define Package/udx-usb/install
	$(INSTALL_DIR) $(1)
	$(CP) $(PKG_BUILD_DIR)/packages/udx-usb/files/. $(1)/
endef

$(eval $(call BuildPackage,udx-usb))
