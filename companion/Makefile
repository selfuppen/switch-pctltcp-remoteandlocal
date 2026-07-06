#---------------------------------------------------------------------------------
# Makefile for the foreground grant companion NRO
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := pctltcp-grant
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include

APP_TITLE   := Pctl Grant
APP_AUTHOR  := pctltcp
APP_VERSION := 1.0.0

ARCH := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS += $(INCLUDE) -D__SWITCH__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS := -lnx
LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NACPFLAGS := --title "$(APP_TITLE)" --author "$(APP_AUTHOR)" --version "$(APP_VERSION)"

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $@ -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nacp

else

.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

%.bin.o	%_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
