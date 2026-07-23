ifeq ($(filter host-test,$(MAKECMDGOALS)),host-test)
.PHONY: host-test
host-test:
	@sh tests/run-host-tests.sh
else
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Install devkitPro switch-dev and export DEVKITPRO.)
endif
ifeq ($(strip $(PLUTONIUM_PREFIX)),)
$(error PLUTONIUM_PREFIX is not set. Point it at a staged Plutonium prefix.)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := nxgallery
BUILD       := build
SOURCES     := source
DATA        := payload/switch/nxgallery/openssl
INCLUDES    := include $(PLUTONIUM_PREFIX)/include
SWITCH_CURL_PREFIX ?= $(PORTLIBS)
SWITCH_OPENSSL_PREFIX ?= $(PORTLIBS)
PLAYBACK_PREFIX ?=
ifneq ($(strip $(PLAYBACK_PREFIX)),)
PLAYBACK_AAC_DECODER := $(shell $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-nm -g \
	$(PLAYBACK_PREFIX)/lib/libavcodec.a 2>/dev/null | grep -c 'ff_aac_decoder')
ifeq ($(PLAYBACK_AAC_DECODER),0)
$(error PLAYBACK_PREFIX must provide an FFmpeg libavcodec with the AAC decoder enabled. Run scripts/build-switch-ffmpeg.sh)
endif
endif
APP_TITLE   := NX Gallery
APP_AUTHOR  := LPFchan
APP_VERSION ?= 0.1.5
APP_ICON    := $(CURDIR)/icon.jpg

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -fPIE
CFLAGS   := -g -Wall -Wextra -Wpedantic -O2 -DNDEBUG -ffunction-sections -fdata-sections $(ARCH)
CFLAGS   += $(INCLUDE)
CXXFLAGS := $(CFLAGS) -std=gnu++17 -D__SWITCH__ -D_DEFAULT_SOURCE=1 -DCURL_STATICLIB
CXXFLAGS += -DNXGALLERY_VERSION=\"$(APP_VERSION)\"
CXXFLAGS += -fno-rtti
ifeq ($(NXGALLERY_AUTOMATION_BUILD),1)
CXXFLAGS += -DNXGALLERY_AUTOMATION_BUILD
endif
ifeq ($(NXGALLERY_UPDATE_E2E_BUILD),1)
CXXFLAGS += -DNXGALLERY_AUTOMATION_BUILD -DNXGALLERY_UPDATE_E2E_BUILD
endif
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections
LIBS     := -lpu -lSDL2_ttf -Wl,--start-group -lharfbuzz -lfreetype -Wl,--end-group \
            -lbz2 -lSDL2_image -lpng16 -lz -ljpeg -lwebp -lSDL2_gfx -lSDL2 \
            -lEGL -lglapi -ldrm_nouveau -lcurl -ljson-c -lssl -lcrypto \
            -lm -lstdc++ -lnx -lpthread
ifneq ($(strip $(PLAYBACK_PREFIX)),)
LIBS     := -lavformat -lavcodec -lswscale -lswresample -lavutil $(LIBS)
endif
LIBDIRS  := $(PLUTONIUM_PREFIX) $(PORTLIBS) $(LIBNX)
LIBDIRS  += $(SWITCH_CURL_PREFIX) $(SWITCH_OPENSSL_PREFIX)
ifneq ($(strip $(PLAYBACK_PREFIX)),)
LIBDIRS  += $(PLAYBACK_PREFIX)
endif

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
	$(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(if $(filter /%,$(dir)),$(dir),$(CURDIR)/$(dir))) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export ARCH CFLAGS CXXFLAGS LDFLAGS LIBS
export CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))
export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export OFILES   := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))
export HFILES   := $(foreach dir,$(INCLUDES),$(wildcard $(dir)/*.h) $(wildcard $(dir)/*.hpp))
export LD       := $(CXX)
export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: all automation clean $(BUILD)
all: $(BUILD)
automation:
	@$(MAKE) --no-print-directory NXGALLERY_AUTOMATION_BUILD=1 \
		TARGET=nxgallery-automation BUILD=build-automation all
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -rf build build-automation nxgallery.nro nxgallery.nacp nxgallery.elf \
		nxgallery-automation.nro nxgallery-automation.nacp nxgallery-automation.elf
else
DEPENDS := $(OFILES:.o=.d)
.PHONY: all
all: $(OUTPUT).nro
$(OFILES_SRC): $(HFILES_BIN)
%.pem.o %_pem.h: %.pem
	@$(bin2o)
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
include $(DEVKITPRO)/libnx/switch_rules
-include $(DEPENDS)
endif
endif
