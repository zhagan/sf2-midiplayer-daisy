TARGET = SF2MidiPlayer

LIBDAISY_DIR = ../../libDaisy/
DAISYSP_DIR  = ../../DaisySP/

LDSCRIPT = $(LIBDAISY_DIR)/core/STM32H750IB_qspi.lds

CPP_SOURCES = \
  src/main.cpp \
  src/sd_mount.cpp \
  src/synth_tsf.cpp \
  src/smf_player.cpp \
  src/clock_sync.cpp

C_SOURCES = \
  $(LIBDAISY_DIR)/Middlewares/Third_Party/FatFs/src/option/unicode.c

CXXFLAGS += -Isrc
CFLAGS   += -Isrc

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
