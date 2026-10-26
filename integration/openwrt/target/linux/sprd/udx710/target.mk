# SPDX-License-Identifier: GPL-2.0-only
ARCH:=aarch64
SUBTARGET:=udx710
BOARDNAME:=UDX710 boards
CPU_TYPE:=cortex-a55
define Target/Description
  Build OpenWrt for SY108 using the UDX710 SoC.
endef

# Linux 7.3 removed ALSA's OSS emulation. Keep OpenWrt's sound-core package
# aligned with the ALSA objects supplied by this target kernel.
SOUNDCORE_LOAD = soundcore snd snd-hwdep snd-seq-device snd-rawmidi snd-timer snd-pcm snd-compress
SOUNDCORE_FILES = \
	$(LINUX_DIR)/sound/soundcore.ko \
	$(LINUX_DIR)/sound/core/snd.ko \
	$(LINUX_DIR)/sound/core/snd-hwdep.ko \
	$(LINUX_DIR)/sound/core/snd-seq-device.ko \
	$(LINUX_DIR)/sound/core/snd-rawmidi.ko \
	$(LINUX_DIR)/sound/core/snd-timer.ko \
	$(LINUX_DIR)/sound/core/snd-pcm.ko \
	$(LINUX_DIR)/sound/core/snd-compress.ko
