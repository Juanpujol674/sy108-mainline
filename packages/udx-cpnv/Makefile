SCAN_DEPS := ../source.mk
SY108_RECIPE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
include $(TOPDIR)/rules.mk
PKG_NAME:=udx-cpnv
PKG_VERSION:=3.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only BSD-2-Clause
PKG_BUILD_DEPENDS:=openssl
SY108_SOURCE_PATHS := packages/udx-cpnv components/cpnv
include $(SY108_RECIPE_DIR)../source.mk
include $(INCLUDE_DIR)/package.mk

Build/Prepare = $(call SY108/Prepare,$(PKG_BUILD_DIR))

define Package/udx-cpnv
  SECTION:=net
  CATEGORY:=Network
  TITLE:=UDX710 original-NAND CP and NV service
  DEPENDS:=+procd +libopenssl
endef

define Package/udx-cpnv/conffiles
/etc/udx-cpnv-warm/profile
endef

define Build/Compile
	CPNV_TARGET_SDK=1 CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" \
	 bash $(PKG_BUILD_DIR)/components/cpnv/runtime/build.sh $(PKG_BUILD_DIR)/bin
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 -Wno-unused-function \
	 $(PKG_BUILD_DIR)/components/cpnv/storage/launch.c \
	 $(TARGET_LDFLAGS) -o $(PKG_BUILD_DIR)/bin/udx-cpnv-start
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 \
	 -I$(PKG_BUILD_DIR)/components/cpnv/runtime/components/halo/include \
	 $(PKG_BUILD_DIR)/components/cpnv/runtime/launch/halo-provider.c \
	 $(PKG_BUILD_DIR)/components/cpnv/runtime/components/halo/src/halo_rsa_provider.c \
	 $(PKG_BUILD_DIR)/components/cpnv/runtime/components/halo/src/halo_material.c \
	 $(TARGET_LDFLAGS) -lcrypto -o $(PKG_BUILD_DIR)/bin/udx-halo-provider
endef

define Package/udx-cpnv/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/libexec $(1)/etc/udx-cpnv-warm
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/udx-cpnv-parent $(PKG_BUILD_DIR)/bin/udx-modem-control-warm $(PKG_BUILD_DIR)/bin/udx-refnotifyd $(PKG_BUILD_DIR)/bin/udx-cpnv-start $(PKG_BUILD_DIR)/bin/udx-halo-provider $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/udx-nv-broker-v4 $(PKG_BUILD_DIR)/bin/udx-nv-continuous-v4 $(1)/usr/libexec/
	$(CP) $(PKG_BUILD_DIR)/packages/udx-cpnv/files/. $(1)/
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/packages/udx-cpnv/files/etc/udx-cpnv-warm/profile $(1)/etc/udx-cpnv-warm/profile
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/packages/udx-cpnv/files/etc/udx-refnotify/owner-policy $(1)/etc/udx-refnotify/owner-policy
	$(INSTALL_DIR) $(1)/usr/share/licenses/udx-cpnv
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/components/cpnv/storage/third_party/lz4/LICENSE $(1)/usr/share/licenses/udx-cpnv/lz4
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/components/cpnv/halo/halo-private.bin $(PKG_BUILD_DIR)/components/cpnv/halo/halo-verification.bin $(1)/etc/udx-cpnv-warm/
endef
$(eval $(call BuildPackage,udx-cpnv))
