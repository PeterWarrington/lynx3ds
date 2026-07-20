#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
LYNXDIR   := vendor/lynx

TARGET    := lynx3ds
BUILD     := build
SOURCES   := source $(LYNXDIR)/src $(LYNXDIR)/src/chrtrans $(LYNXDIR)/WWW/Library/Implementation
DATA      := data
GRAPHICS  := gfx
GFXBUILD  := $(BUILD)
INCLUDES  := include $(LYNXDIR) $(LYNXDIR)/src $(LYNXDIR)/src/chrtrans $(LYNXDIR)/WWW/Library/Implementation
ROMFS     := romfs

#---------------------------------------------------------------------------------
# Lynx source files that are for other platforms (DOS/Windows/VMS), or are
# fallback implementations for libc functions newlib already provides.
# This list was derived by diffing against a real native (macOS) Lynx build's
# link line -- these are exactly the files *not* linked into a working lynx.
#---------------------------------------------------------------------------------
EXCLUDE_C := Xsystem.c mktime.c strstr.c wcwidth.c tidy_tls.c makeuctb.c \
             dtd_util.c HTVMS_WaisProt.c HTVMS_WaisUI.c HTVMSUtils.c HTWAIS.c

#---------------------------------------------------------------------------------
# Metadata baked into the .smdh (shows up on the HOME Menu / Homebrew Launcher).
# 3ds_rules (included above) turns these into the smdhtool call; ICON is
# optional -- if unset it falls back to TARGET.png/icon.png if present, or a
# default devkitPro icon otherwise.
#---------------------------------------------------------------------------------
APP_TITLE       := Lynx 3DS
APP_DESCRIPTION := A beautiful text-mode web browser.
APP_AUTHOR      := lilpete.me/lynx3ds

#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__ -DHAVE_CONFIG_H -DNOUSERS -D_GNU_SOURCE

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lcitro2d -lcitro3d -lmbedtls -lmbedx509 -lmbedcrypto -lctru -lm

#---------------------------------------------------------------------------------
PORTLIBS := $(DEVKITPRO)/portlibs/3ds
LIBDIRS	:= $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CFILES		:=	$(filter-out $(EXCLUDE_C),$(CFILES))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))

#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
ifeq ($(GFXBUILD),$(BUILD))
#---------------------------------------------------------------------------------
export T3XFILES := $(GFXFILES:.t3s=.t3x)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
# Builds a CIA (for installing via FBI etc., as an alternative to running the
# .3dsx over 3dslink/Homebrew Launcher). Needs $(TARGET).elf already built
# (plain `make` first), plus host tools/3dstool, tools/bannertool and
# tools/makerom on PATH under ./tools -- these aren't part of the devkitARM
# toolchain, so build them once from their upstream sources:
#   https://github.com/dnasdw/3dstool
#   https://github.com/diasurgical/bannertool
#   https://github.com/3DSGuy/Project_CTR (makerom subdir)
#---------------------------------------------------------------------------------
cia: all
#---------------------------------------------------------------------------------
	@mkdir -p cia/build
	@echo building romfs.bin ...
	@tools/3dstool -cvtf romfs cia/build/romfs.bin --romfs-dir $(ROMFS)
	@echo building icon.smdh ...
	@tools/bannertool makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i icon.png -o cia/build/icon.smdh
	@echo building banner.bnr ...
	@tools/bannertool makebanner -i banner/banner.png -a banner/banner.wav -o cia/build/banner.bnr
	@echo building $(TARGET).cia ...
	@tools/makerom -f cia -o $(TARGET).cia -rsf cia/template.rsf -target t -exefslogo \
		-icon cia/build/icon.smdh -banner cia/build/banner.bnr -elf $(TARGET).elf \
		-romfs cia/build/romfs.bin -major 0 -minor 1 -micro 4

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PRECIOUS	:	%.t3x
#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
