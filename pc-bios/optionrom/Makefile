all: build-all
# Dummy command so that make thinks it has done something
	@true

include ../../config-host.mak
include $(SRC_PATH)/rules.mak

$(call set-vpath, $(SRC_PATH)/pc-bios/optionrom)

.PHONY : all clean build-all

CFLAGS := -Wall -Wstrict-prototypes -Werror -fomit-frame-pointer -fno-builtin
CFLAGS += -I$(SRC_PATH)
CFLAGS += $(call cc-option, $(CFLAGS), -fno-stack-protector)
QEMU_CFLAGS = $(CFLAGS)

build-all: multiboot.stamp linuxboot.stamp
.SECONDARY:

# suppress auto-removal of intermediate files
.SECONDARY:

%.img: %.o
	$(call quiet-command,$(LD) -Ttext 0 -e _start -s -o $@ $<,"  Building $(TARGET_DIR)$@")

%.raw: %.img
	$(call quiet-command,$(OBJCOPY) -O binary -j .text $< $@,"  Building $(TARGET_DIR)$@")

%.bin: %.raw
	$(call quiet-command,$(SHELL) $(SRC_PATH)/scripts/signrom.sh $< $@,"  Signing $(TARGET_DIR)$@")

%.stamp: %.S %.bin
	$(call quiet-command,:,"  GEN   $@")
	@sha1=`sha1sum < $< | awk '{print $$1}'`; \
	if test -f "$@" && test "`cat $@`" = "$$sha1"; then \
	  echo '$@ is unchanged'; \
	else \
	  echo 'Updating $(TARGETDIR)../$*.bin'; \
	  cp $*.bin ..; \
	fi; \
	echo $$sha1 > $@

clean:
	rm -f *.o *.d *.raw *.img *.bin *~
