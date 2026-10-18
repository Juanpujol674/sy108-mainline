# SPDX-License-Identifier: GPL-2.0-only
# Prepare only this package's local sources; OpenWrt tracks their changes.
SY108_PACKAGE_DIR:=$(realpath $(dir $(lastword $(MAKEFILE_LIST))))
SY108_PROJECT_DIR:=$(realpath $(SY108_PACKAGE_DIR)/..)
SY108_BUILD_ID?=$(shell git -C $(SY108_PROJECT_DIR) describe --always --dirty)
PKG_FILE_DEPENDS += $(addprefix $(SY108_PROJECT_DIR)/,$(SY108_SOURCE_PATHS))

define SY108/Prepare
	$(INSTALL_DIR) $(1)
	set -e; $(foreach path,$(SY108_SOURCE_PATHS),$(INSTALL_DIR) $(1)/$(dir $(path)); $(CP) $(SY108_PROJECT_DIR)/$(path) $(1)/$(dir $(path));)
endef
