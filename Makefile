# =============================================================================
# Audiothurgist — Hermetic Modular Alchemy Lab firmware.
# The final form of BreakFix: a sampling / slicing / rearranging toolkit.
#
# Expects sibling clones in the parent directory (like alchemy-template):
#   ../alchemy-sdk   (github.com/hermetic-modular/alchemy-sdk)
#   ../DaisySP       (github.com/electro-smith/DaisySP, for DaisySP-LGPL reverb)
#
#   make libdaisy      — build the SDK's vendored libDaisy once after cloning
#   make               — build firmware (BOARD=v2)
#   make program-dfu   — flash (module parked in the Daisy bootloader)
#   make program-live  — reboot the running module over USB and flash it
# =============================================================================

TARGET = audiothurgist

BOARD ?= v2

ALCHEMY_DIR  = ../alchemy-sdk
LIBDAISY_DIR = $(ALCHEMY_DIR)/vendor/libDaisy
DAISYSP_LGPL = ../DaisySP/DaisySP-LGPL

CPP_SOURCES = src/audiothurgist.cpp src/audiothurgist_manual.cpp
CPP_SOURCES += $(sort $(shell find $(ALCHEMY_DIR)/framework/src -name '*.cpp'))
CPP_SOURCES += $(sort $(wildcard $(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/src/*.cpp))
CPP_SOURCES += $(DAISYSP_LGPL)/Source/Effects/reverbsc.cpp

C_INCLUDES += \
    -Isrc \
    -Icore \
    -Ivendor/capicola/lib \
    -I$(ALCHEMY_DIR)/framework/include \
    -I$(ALCHEMY_DIR)/hardware/include \
    -I$(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/include \
    -I$(DAISYSP_LGPL)/Source \
    -I$(LIBDAISY_DIR)/Middlewares/Third_Party/FatFs/src

USE_FATFS = 1

ifeq ($(BOARD),v2)
C_DEFS += -DALCHEMY_BOARD_V2
C_DEFS += -DALCHEMY_LOCK_INLINE_RAM_LIMIT=0 -DBB_SCRATCH_EXTERNAL   # DTCM relief (review M7)
endif
# SCOPE: HostLink max_body 512 → 1024 (protocol §1 allows up to 2048; hosts read
# it from HELLO, which itself always fits 512, and never ask for more than they
# can take). A four-channel 0x62 frame is header + 4 × n bytes: 125 samples per
# channel at 512, 253 at 1024 — one request per scope frame instead of two.
# The SDK sizes its CDC rings and frame buffers from this, all in DTCM (.bss):
# 1024 costs ~6 KB there, 2048 would cost ~18 KB and leave the stack ~17 KB.
C_DEFS += -DALCHEMY_HOSTLINK_MAX_BODY=1024

APP_TYPE = BOOT_SRAM
# Local linker adds .axi_bss in AXI SRAM — upstream's SRAM linker leaves it
# undefined, orphaning the SDK's SDMMC FATFS into DMA-unreachable DTCM.
LDSCRIPT = src/audiothurgist.lds

CPP_STANDARD = -std=gnu++17

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

BOARD_STAMP := $(BUILD_DIR)/.board-$(BOARD)
ifeq ($(wildcard $(BOARD_STAMP)),)
_BOARD_GUARD := $(shell rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.d $(BUILD_DIR)/*.lst $(BUILD_DIR)/.board-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(BOARD_STAMP))
endif

.PHONY: libdaisy
libdaisy:
	$(MAKE) -C $(LIBDAISY_DIR)

USBPID ?= df11
.PHONY: program-live
program-live: all
	node $(ALCHEMY_DIR)/tools/hostlink-cli/hostlink.mjs reboot bootloader
	dfu-util -w -a 0 -s $(FLASH_ADDRESS):leave -D $(BUILD_DIR)/$(TARGET_BIN) -d ,0483:$(USBPID)
