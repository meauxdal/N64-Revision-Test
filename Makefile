N64_INST   = /n64_toolchain
PROG_NAME  = n64_revision_test

all: $(PROG_NAME).z64

OBJS = main.o

include $(N64_INST)/include/n64.mk

$(PROG_NAME).elf: $(OBJS)

clean:
	rm -f *.o *.elf *.z64

.PHONY: all clean
