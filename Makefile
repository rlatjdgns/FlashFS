TARGET = flashfs
CC = arm-none-eabi-g++
CFLAGS = -mcpu=cortex-m3 -mthumb -nostdlib -nostartfiles -ffreestanding -fno-exceptions -fno-unwind-tables -O0 -g
LDFLAGS = -T linker/stm32f103.ld -nostdlib

SRCS = src/startup.cpp src/main.cpp src/uart.cpp src/spi.cpp src/flash.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET).elf

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.cpp
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET).elf

flash:
	openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "program $(TARGET).elf verify reset exit"