TARGET = SF2MidiPlayer

# Build for Daisy Bootloader (store/execute app from QSPI, not internal FLASH)
APP_TYPE = BOOT_QSPI

# DEBUG = 0
# OPT   = -Os
# LTO   = 1
USE_FATFS = 1

CPP_SOURCES = \
  src/main.cpp \
  src/sd_mount.cpp \
  src/synth_tsf.cpp \
  src/smf_player.cpp \

LIBDAISY_DIR = ../../libDaisy/
DAISYSP_DIR  = ../../DaisySP/

# Generate a link map to inspect size/symbol pulls
# LDFLAGS += -Wl,-Map=build/$(TARGET).map,--cref
# LDSCRIPT = ./alt_sram.lds

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
