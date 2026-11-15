# ============================================================================
# Minimalist Makefile for ohPlayer
# ============================================================================

OSPLATFORM = raspbian
TARGET     = $(OSPLATFORM)/openhome-player
CXX        = clang++

# Architecture Target Definitions
TARG_ARCH  = aarch64-scarthgap-linux
BUILD_TYPE = Release
OBJ_DIR    = $(OSPLATFORM)/objs


KFR_DIR       ?= ../dependencies/$(TARG_ARCH)/KFR-7.2.0-Linux-ARM64
CAPTURE_ALSA  ?= 1

BUILD_FLAGS = -std=c++20 -fstack-protector -fstack-check -DDEFINE_LITTLE_ENDIAN -fpermissive -DENABLE_KFR_DSP
ifeq ($(BUILD_TYPE), Release)
	BUILD_FLAGS += -O3 -march=native -ffast-math -fno-finite-math-only -DNDEBUG
else
	BUILD_FLAGS += -Og -g -gdwarf-4 -Wall -Wextra -DDEBUG
endif

# Core Compiler Optimization and Assembly Flags
CFLAGS = -c $(BUILD_FLAGS) -DTARG_ARCH=$(TARG_ARCH) \
         $(shell pkg-config --cflags glib-2.0)

# Native Installation Path Configurations
PREFIX      = /usr
BINDIR      = $(PREFIX)/bin
RESDIR      = $(PREFIX)/share/openhome-player
DOCDIR      = $(PREFIX)/share/doc/openhome-player
RESOURCEDIR = res
DEPRESOURCEDIR = ../dependencies/$(TARG_ARCH)/ohMediaPlayer/res

LIBS = $(shell pkg-config --libs glib-2.0) \
       -lConfigUiTestUtils -lConfigUi -lSourcePlaylist -lSourceSongcast \
       -lSourceUpnpAv -lPodcast -lSourceRadio -lSourceScd -lohMediaPlayer \
       -lWebAppFramework $(DEPS_DIR)/ohNet/lib/libohNetCore.a \
       -lCodecAifc \
       -lCodecAlacApple -lCodecAlacAppleBase -lCodecPcm -lCodecAiff \
       -lCodecAiffBase -lCodecVorbis -llibOgg -lCodecFlac -lCodecWav \
       -lCodecDsdDsf -lCodecDsdDff -lCodecDsdRaw -lSourceRaop -lohPipeline \
       -lCodecAacFdkAdts -lCodecAacFdkBase -lCodecMp3 -lCodecAacFdkMp4 -lCodecAacFdk \
       -lkfr_dft -lkfr_dsp \
       -lpthread -lasound -ldl -lm -lOdp -lssl -lcrypto

# Static Search Path Directories
DEPS_DIR = ../dependencies/$(TARG_ARCH)
INCLUDES = -I$(DEPS_DIR)/ohMediaPlayer/include \
           -I$(DEPS_DIR)/ohNetmon/include \
           -I$(DEPS_DIR)/ohNetGenerated-$(TARG_ARCH)-$(BUILD_TYPE)/include/ohnet/OpenHome/Net/Core \
           -I$(DEPS_DIR)/ohNet/include/ohnet \
           -I$(DEPS_DIR) \
           -I$(KFR_DIR)/include

LDFLAGS  = -L$(DEPS_DIR)/ohMediaPlayer/lib \
           -L$(DEPS_DIR)/ohNetmon/lib \
           -L$(DEPS_DIR)/ohNet/lib \
           -L$(DEPS_DIR)/ohNetGenerated-$(TARG_ARCH)-$(BUILD_TYPE)/lib \
           -L$(KFR_DIR)/lib

# Workspace Source Track Aggregations
SOURCES  = $(filter-out SoxrProcessor.cpp KfrDspProcessor.cpp KfrDspFilter.cpp KfrDspFilterImpl.cpp, $(wildcard *.cpp))
ifeq ($(CAPTURE_ALSA),1)
BUILD_FLAGS += -DCAPTURE_ALSA=1
SOURCES += $(wildcard capture/*.cpp)
endif
OBJECTS  = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES))
HEADERS  = $(wildcard *.h capture/*.h)

# ============================================================================
# Compilation Build Automation Targets
# ============================================================================

.PHONY: default all clean build install uninstall

default: build $(TARGET)
all: default

$(OBJ_DIR)/%.o: %.cpp $(HEADERS)
	$(CXX) $(CFLAGS) $(INCLUDES) -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CXX) -Wall $(LDFLAGS) -Wl,--start-group $(OBJECTS) $(LIBS) -Wl,--end-group -o $@

build: ; @mkdir -p $(OBJ_DIR)/capture

clean:
	rm -rf $(OSPLATFORM)/objs
	rm -f $(TARGET)

install:
	install -m 755 -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(DOCDIR) $(DESTDIR)$(RESDIR) $(DESTDIR)$(RESDIR)/res
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)
	install -m 644 OpenHome-48x48.png $(DESTDIR)$(RESDIR)
	install -m 644 copyright $(DESTDIR)$(DOCDIR)
	cp -R $(RESOURCEDIR) $(DESTDIR)$(RESDIR)
	cp $(DEPRESOURCEDIR)/lang/en-gb/ConfigOptions.txt $(DESTDIR)$(RESDIR)/res/lang

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/openhome-player
	rm -rf $(DESTDIR)$(DOCDIR)
	rm -rf $(DESTDIR)$(RESDIR)
