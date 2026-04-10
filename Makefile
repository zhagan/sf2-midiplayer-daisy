TARGET = SF2MidiPlayer

# Build for Daisy Bootloader (store/execute app from QSPI, not internal FLASH)
APP_TYPE = BOOT_QSPI

# DEBUG = 0
# OPT   = -Os
# LTO   = 1
USE_FATFS = 1
USE_DAISYSP_LGPL = 1

CPP_SOURCES = \
  src/main.cpp \
  src/sd_mount.cpp \
  src/synth_tsf.cpp \
  src/smf_player.cpp \
  src/major_midi_settings.cpp \
  src/media_library.cpp \
  src/clock_sync.cpp \
  src/cv_gate_persist.cpp \
  src/midi_routing_persist.cpp \
  src/cv_gate_engine.cpp \
  src/ui_input.cpp \
  src/ui_controller.cpp \
  src/ui_renderer.cpp \
  src/mixer_transport.cpp

LIBDAISY_DIR = ../../libDaisy/
DAISYSP_DIR  = ../../DaisySP/

# Generate a link map to inspect size/symbol pulls
# LDFLAGS += -Wl,-Map=build/$(TARGET).map,--cref
LDSCRIPT = ./alt_sram.lds

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
