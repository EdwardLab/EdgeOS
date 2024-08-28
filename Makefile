# assembler
ASM = nasm
# compiler
CC = gcc
# linker
LD = ld
# grub iso creator
GRUB = grub-mkrescue
# sources
SRC = src
ASM_SRC = $(SRC)/asm
# objects
OBJ = obj
ASM_OBJ = $(OBJ)/asm
CONFIG = ./config
OUT = out
INC = ./include
INCLUDE = -I$(INC)

MKDIR = mkdir -p
CP = cp -f
DEFINES =

# build time get for uname
COMPILE_TIME := $(shell date +"%a %b %d %H:%M:%S %Z %Y")
CFLAGS := $(INCLUDE) $(DEFINES) -DCOMPILE_TIME="\"$(COMPILE_TIME)\"" -m32 -std=gnu99 -ffreestanding -Wall -Wextra

# assembler flags
ASM_FLAGS = -f elf32
# linker flags, for linker add linker.ld file too
LD_FLAGS = -m elf_i386 -T $(CONFIG)/linker.ld -nostdlib

# target file to create in linking
TARGET = $(OUT)/edgeos.bin

# iso file target to create
TARGET_ISO = $(OUT)/edgeos.iso
ISO_DIR = $(OUT)/isodir

OBJECTS = $(ASM_OBJ)/entry.o $(ASM_OBJ)/load_gdt.o\
          $(ASM_OBJ)/load_idt.o $(ASM_OBJ)/exception.o $(ASM_OBJ)/irq.o\
          $(OBJ)/io_ports.o $(OBJ)/vga.o\
          $(OBJ)/string.o $(OBJ)/console.o\
          $(OBJ)/gdt.o $(OBJ)/idt.o $(OBJ)/isr.o $(OBJ)/8259_pic.o\
          $(OBJ)/keyboard.o\
          $(OBJ)/kernel.o\
		  $(OBJ)/stdio.o\
		  $(OBJ)/fs.o

all: $(OBJECTS)
	@printf "[ linking... ]\n"
	$(LD) $(LD_FLAGS) -o $(TARGET) $(OBJECTS)
	grub-file --is-x86-multiboot $(TARGET)
	@printf "\n"
	@printf "[ building ISO... ]\n"
	$(MKDIR) $(ISO_DIR)/boot/grub
	$(CP) $(TARGET) $(ISO_DIR)/boot/
	$(CP) $(CONFIG)/grub.cfg $(ISO_DIR)/boot/grub/
	$(GRUB) -o $(TARGET_ISO) $(ISO_DIR)
	rm -f $(TARGET)

$(ASM_OBJ)/entry.o : $(ASM_SRC)/entry.asm
	@printf "[ $(ASM_SRC)/entry.asm ]\n"
	$(ASM) $(ASM_FLAGS) $(ASM_SRC)/entry.asm -o $(ASM_OBJ)/entry.o
	@printf "\n"

$(ASM_OBJ)/load_gdt.o : $(ASM_SRC)/load_gdt.asm
	@printf "[ $(ASM_SRC)/load_gdt.asm ]\n"
	$(ASM) $(ASM_FLAGS) $(ASM_SRC)/load_gdt.asm -o $(ASM_OBJ)/load_gdt.o
	@printf "\n"

$(ASM_OBJ)/load_idt.o : $(ASM_SRC)/load_idt.asm
	@printf "[ $(ASM_SRC)/load_idt.asm ]\n"
	$(ASM) $(ASM_FLAGS) $(ASM_SRC)/load_idt.asm -o $(ASM_OBJ)/load_idt.o
	@printf "\n"

$(ASM_OBJ)/exception.o : $(ASM_SRC)/exception.asm
	@printf "[ $(ASM_SRC)/exception.asm ]\n"
	$(ASM) $(ASM_FLAGS) $(ASM_SRC)/exception.asm -o $(ASM_OBJ)/exception.o
	@printf "\n"

$(ASM_OBJ)/irq.o : $(ASM_SRC)/irq.asm
	@printf "[ $(ASM_SRC)/irq.asm ]\n"
	$(ASM) $(ASM_FLAGS) $(ASM_SRC)/irq.asm -o $(ASM_OBJ)/irq.o
	@printf "\n"

$(OBJ)/io_ports.o : $(SRC)/io_ports.c
	@printf "[ $(SRC)/io_ports.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/io_ports.c -o $(OBJ)/io_ports.o
	@printf "\n"

$(OBJ)/vga.o : $(SRC)/vga.c
	@printf "[ $(SRC)/vga.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/vga.c -o $(OBJ)/vga.o
	@printf "\n"

$(OBJ)/string.o : $(SRC)/string.c
	@printf "[ $(SRC)/string.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/string.c -o $(OBJ)/string.o
	@printf "\n"

$(OBJ)/console.o : $(SRC)/console.c
	@printf "[ $(SRC)/console.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/console.c -o $(OBJ)/console.o
	@printf "\n"

$(OBJ)/gdt.o : $(SRC)/gdt.c
	@printf "[ $(SRC)/gdt.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/gdt.c -o $(OBJ)/gdt.o
	@printf "\n"

$(OBJ)/idt.o : $(SRC)/idt.c
	@printf "[ $(SRC)/idt.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/idt.c -o $(OBJ)/idt.o
	@printf "\n"

$(OBJ)/isr.o : $(SRC)/isr.c
	@printf "[ $(SRC)/isr.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/isr.c -o $(OBJ)/isr.o
	@printf "\n"

$(OBJ)/8259_pic.o : $(SRC)/8259_pic.c
	@printf "[ $(SRC)/8259_pic.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/8259_pic.c -o $(OBJ)/8259_pic.o
	@printf "\n"

$(OBJ)/keyboard.o : $(SRC)/keyboard.c
	@printf "[ $(SRC)/keyboard.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/keyboard.c -o $(OBJ)/keyboard.o
	@printf "\n"

$(OBJ)/kernel.o : $(SRC)/kernel.c
	@printf "[ $(SRC)/kernel.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/kernel.c -o $(OBJ)/kernel.o
	@printf "\n"

$(OBJ)/stdio.o : $(SRC)/stdio.c
	@printf "[ $(SRC)/stdio.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/stdio.c -o $(OBJ)/stdio.o
	@printf "\n"

$(OBJ)/fs.o : $(SRC)/fs/fs.c
	@printf "[ $(SRC)/fs/fs.c ]\n"
	$(CC) $(CFLAGS) -c $(SRC)/fs/fs.c -o $(OBJ)/fs.o
	@printf "\n"

clean:
	rm -f $(OBJ)/*.o
	rm -f $(ASM_OBJ)/*.o
	rm -rf $(OUT)/*
