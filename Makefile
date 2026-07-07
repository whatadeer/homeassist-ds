#---------------------------------------------------------------------------------
# Home Assistant 3DS Remote
#
# Requires devkitARM + libctru + citro2d/citro3d + curl/mbedtls/jansson/zlib
# ports for 3DS, plus makerom/bannertool for CIA packaging. Easiest path:
# `podman build -t ha3ds-builder -f Containerfile .` then `.\build.ps1` -
# see README.md. This Makefile itself is unchanged either way.
#
# `make`     builds build/ha3ds.3dsx (run via Homebrew Launcher / Citra)
# `make cia` builds ha3ds.cia (sideload via FBI on CFW)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)

ifeq ($(wildcard $(TOPDIR)/source/config.h),)
$(error source/config.h is missing. Copy source/config.h.example to source/config.h and fill in your Home Assistant URL/token)
endif

include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing header files
#---------------------------------------------------------------------------------
TARGET		:=	ha3ds
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	source

APP_TITLE		:=	HA Remote
APP_DESCRIPTION	:=	Home Assistant Remote Control
APP_AUTHOR		:=	Homebrew
APP_PRODUCT_CODE	:=	CTR-P-HA3D
APP_UNIQUE_ID		:=	0xFF3D0

ICON := meta/icon.png

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# Static link order matters: -lctru must come AFTER the libraries that need
# its socket/ssl symbols (curl, mbedcrypto), or ld's single left-to-right
# pass never pulls those object files in.
LIBS	:=	-lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ljansson -lz -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

#---------------------------------------------------------------------------------
export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES		:=	$(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

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

#---------------------------------------------------------------------------------
# CIA packaging: banner (image+audio) and icon (reuses the .smdh built above
# for the .3dsx) get combined via makerom. Lives in this (outer) branch since
# it depends on files the recursive `all` build below produces.
#---------------------------------------------------------------------------------
BANNER_IMAGE	:=	$(TOPDIR)/meta/banner.png
BANNER_AUDIO	:=	$(TOPDIR)/meta/audio.wav

banner.bnr: $(BANNER_IMAGE) $(BANNER_AUDIO)
	@bannertool makebanner -i $(BANNER_IMAGE) -a $(BANNER_AUDIO) -o banner.bnr
	@echo built ... $(notdir $@)

$(TARGET).cia: all banner.bnr
	@makerom -f cia -o $(TARGET).cia -rsf $(TOPDIR)/resources/app.rsf -target t -exefslogo \
		-icon $(TARGET).smdh -banner banner.bnr -elf $(TARGET).elf \
		-DAPP_TITLE="$(APP_TITLE)" -DAPP_PRODUCT_CODE="$(APP_PRODUCT_CODE)" -DAPP_UNIQUE_ID="$(APP_UNIQUE_ID)"
	@echo built ... $(notdir $@)

.PHONY: all clean cia cia-clean

#---------------------------------------------------------------------------------
all: $(BUILD) $(DEPSDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

cia: $(TARGET).cia

$(BUILD):
	@mkdir -p $@

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

cia-clean:
	@rm -f banner.bnr $(TARGET).cia

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
