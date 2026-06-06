#include <stdint.h>
void uart_init();
void uart_send_string(const char* s);
void uart_send_byte(uint8_t data);
void spi_init();
void fs_init();
void fs_create(const char* filename);
void fs_write(uint8_t file_id, uint8_t* data, uint32_t length);
void fs_read(uint8_t file_id, uint8_t* buffer, uint32_t length);

int main(){
    *(volatile uint32_t*)0x40021018 |= (1<<4);
    *(volatile uint32_t*)0x40011004 &= ~(0xF<<20);
    *(volatile uint32_t*)0x40011004 |= (0x3<<20);

    uart_init();
    uart_send_string("STM32 booted\n");
    spi_init();
    uart_send_string("SPI done\n");
    fs_init();
    uart_send_string("fs_init done\n");
    fs_create("sensor.bin");
    uart_send_string("fs_create done\n");

    uint8_t write_data[10] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    fs_write(0, write_data, 10);
    uart_send_string("fs_write done\n");

    uint8_t read_data[10] = {0};
    fs_read(0, read_data, 10);
    uart_send_string("fs_read done\n");
    for(int i = 0; i < 10; i++){
        uart_send_byte(read_data[i]);
    }
    uart_send_byte('\n');

    while(true){
        *(volatile uint32_t*)0x4001100C |= (1<<13);
        for(volatile int i = 0; i < 1000000; i++);
        *(volatile uint32_t*)0x4001100C &= ~(1<<13);
        for(volatile int i = 0; i < 1000000; i++);
    }
    return 0;
}