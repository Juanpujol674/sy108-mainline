# SPDX-License-Identifier: GPL-2.0-only
SCAN_DEPS := ../source.mk
SY108_RECIPE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
include $(TOPDIR)/rules.mk
PKG_NAME:=udx-cpnv
PKG_VERSION:=4.0
PKG_RELEASE:=6
PKG_LICENSE:=GPL-2.0-only MIT
PKG_BUILD_DEPENDS:=openssl liblz4
SY108_SOURCE_PATHS := packages/udx-cpnv components/cpnv
include $(SY108_RECIPE_DIR)../source.mk
include $(INCLUDE_DIR)/package.mk

Build/Prepare = $(call SY108/Prepare,$(PKG_BUILD_DIR))

define Package/udx-cpnv
  SECTION:=net
  CATEGORY:=Network
  TITLE:=UDX710 original-NAND CP and NV service
  DEPENDS:=+procd +libopenssl +liblz4 +libubus +libubox
endef

define Package/udx-cpnv/conffiles
/etc/udx-cpnv-warm/profile
endef

define Build/Compile
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/bin
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -D_GNU_SOURCE -std=c11 \
	 -Wall -Wextra -Werror -I$(PKG_BUILD_DIR)/components/cpnv/include \
	 $(PKG_BUILD_DIR)/components/cpnv/src/*.c $(TARGET_LDFLAGS) \
	 -lubus -lubox -lcrypto -llz4 -pthread -o $(PKG_BUILD_DIR)/bin/udx-cpnvd
endef

define Package/udx-cpnv/install
	$(INSTALL_DIR) $(1)/lib/firmware/sprd
	$(LN) /run/udx-cpnv-warm $(1)/lib/firmware/sprd/udx710-cp
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/udx-cpnv-warm
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/udx-cpnvd $(1)/usr/sbin/
	$(CP) $(PKG_BUILD_DIR)/packages/udx-cpnv/files/. $(1)/
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/packages/udx-cpnv/files/etc/udx-cpnv-warm/profile $(1)/etc/udx-cpnv-warm/profile
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/packages/udx-cpnv/files/etc/udx-refnotify/owner-policy $(1)/etc/udx-refnotify/owner-policy
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/components/cpnv/halo/halo-private.bin $(PKG_BUILD_DIR)/components/cpnv/halo/halo-verification.bin $(1)/etc/udx-cpnv-warm/
endef
$(eval $(call BuildPackage,udx-cpnv))
