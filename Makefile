#---------------------------------------------------------------------------------
# Space Destroyer 3D - Nintendo DS build (devkitARM + libnds/calico + maxmod)
#   make                  -> space_destroyer.nds    (needs gen/, see tools/build_ds_assets.py)
#   make dist             -> dist/space_destroyer.nds
#   make STRESS=1|2|3|4   -> test builds (see PROGRESS.md): 1 = normal mode, speed 5, 300 stars; 2 = hard mode; 3 = manual input;
#                            4 = like 3, lid runs the sleep handlers without the BIOS sleep
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITPRO=/opt/devkitpro DEVKITARM=$$DEVKITPRO/devkitARM")
endif
export GAME_TITLE     := Space Destroyer 3D
export GAME_SUBTITLE1 := by AeEn123
export GAME_SUBTITLE2 := DS port
export GAME_ICON      ?= $(CURDIR)/gen/icon.bmp
include $(DEVKITARM)/ds_rules
# "####" is the homebrew game code: melonDS only attaches DLDI for it, and flashcart kernels may use it to choose homebrew handling.
_ADDFILES += -g "\#\#\#\#" 00 "SPACEDESTROY"

TARGET   := space_destroyer
BUILD    := build
SOURCES  := src
ARCH     := -march=armv5te -mtune=arm946e-s
CFLAGS   := -g -Wall -O2 -std=gnu11 -ffunction-sections -fdata-sections -fno-strict-aliasing $(ARCH) $(INCLUDE) -DARM9 $(EXTRA_CFLAGS)
ASFLAGS  := -g $(ARCH)
# calico's normal layout (ARM9 at 0x02004000, the one that is known to boot on Alfie's flashcart) with a bigger load region.
# The default stops at 0x02280000, the 4 MB mirror of the DSi-only ARM7i load area; the real limit here is the ARM7 at 0x02380000.
LDFLAGS   = -specs=ds9-big.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)
LIBS     := -lfat -lmm9 -lnds9 -lm
LIBDIRS  := $(LIBNDS)
ifneq ($(STRESS),)
EXTRA_CFLAGS += -DSTRESS=$(STRESS)
TARGET   := space_destroyer_stress$(STRESS)
BUILD    := build_stress$(STRESS)
endif
export EXTRA_CFLAGS

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export VPATH   := $(CURDIR)/src $(CURDIR)/gen
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES   := $(notdir $(wildcard src/*.c))
export AUDIOFILES := $(wildcard $(CURDIR)/gen/sfx/*.wav)
export LD      := $(CC)
export OFILES_BIN := soundbank.bin.o textures.bin.o tex_palettes.bin.o music.bin.o
export OFILES_SOURCES := $(CFILES:.c=.o)
export OFILES  := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES  := soundbank_bin.h textures_bin.h tex_palettes_bin.h music_bin.h
export INCLUDE := -I$(CURDIR)/src -I$(CURDIR)/gen -I$(CURDIR)/$(BUILD) $(foreach dir,$(LIBDIRS),-I$(dir)/include)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
.PHONY: all clean dist
all:
	@mkdir -p $(BUILD)
	@sed 's/lma9    : ORIGIN = 0x2004000, LENGTH = 0x27c000/lma9    : ORIGIN = 0x2004000, LENGTH = 0x37c000/' $(CALICO)/lib/ds9.ld > $(BUILD)/ds9-big.ld
	@grep -q 'LENGTH = 0x37c000' $(BUILD)/ds9-big.ld
	@sed 's/ds9\.ld/ds9-big.ld/' $(CALICO)/share/ds9.specs > $(BUILD)/ds9-big.specs
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile BUILD=$(BUILD)
dist: all
	@size=$$($(DEVKITARM)/bin/arm-none-eabi-size -B $(TARGET).elf | awk 'NR==2 {print $$1+$$2}'); echo "ARM9 image about $$size bytes"; \
	  if [ $$size -gt 3600000 ]; then echo "ARM9 image too big: it would overlap the ARM7 load area at 0x02380000"; exit 1; fi
	mkdir -p dist && cp $(TARGET).nds dist/
	python3 tools/set_gamecode.py $(TARGET).nds dist/$(TARGET)_sdde.nds SDDE   # fallback: the header older builds booted with
clean:
	rm -rf build build_stress* *.elf *.nds
else
$(OUTPUT).nds: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
$(OFILES_SOURCES): $(HFILES)
soundbank.bin soundbank.h: $(AUDIOFILES)
	@mmutil $^ -d -osoundbank.bin -hsoundbank.h
%.bin.o %_bin.h: %.bin
	@echo $(notdir $<)
	@$(bin2o)
-include $(DEPSDIR)/*.d
endif
