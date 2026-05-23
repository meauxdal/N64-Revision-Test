N64_INST   = /n64_toolchain
PROG_NAME  = n64_hardware_test

all: $(PROG_NAME).z64

OBJS = main.o

# C++ for std::bit_cast; target MIPS III to allow 64-bit register access in asm
CXXFLAGS += -std=c++20

include $(N64_INST)/include/n64.mk

$(PROG_NAME).elf: $(OBJS)

clean:
	rm -f *.o *.elf *.z64

.PHONY: all clean
