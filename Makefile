DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRU  := $(DEVKITPRO)/libctru
TOOLS := $(DEVKITPRO)/tools/bin
CC    := $(DEVKITARM)/bin/arm-none-eabi-gcc
ARCH  := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
FLAGS := $(ARCH) -O2 -ffast-math -D__3DS__ -specs=3dsx.specs -I$(CTRU)/include -L$(CTRU)/lib

all: hello.3dsx grass.3dsx

%.3dsx: %.elf
	$(TOOLS)/3dsxtool $< $@

hello.elf: main.c
	$(CC) $(FLAGS) $< -lctru -lm -o $@

grass.elf: grass.c shader_shbin.o
	$(CC) $(FLAGS) $^ -lcitro3d -lctru -lm -o $@

shader_shbin.o: shader.v.pica
	$(TOOLS)/picasso -o shader.shbin $<
	$(TOOLS)/bin2s shader.shbin > shader_shbin.s
	$(CC) $(ARCH) -c shader_shbin.s -o $@

clean:
	rm -f *.elf *.3dsx *.shbin shader_shbin.s shader_shbin.o
