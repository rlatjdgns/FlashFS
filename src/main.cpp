#include <stdint.h>
void uart_init();
void uart_send_string(const char* s);
void uart_send_byte(uint8_t data);

void spi_init();
void spi_transmit(uint8_t data);
uint8_t spi_receive();
void spi_nss_low();
void spi_nss_high();

void flash_read_jedec_id();
void flash_write_enable();
void flash_wait_busy();
void flash_sector_erase(uint32_t address);
void flash_page_program(uint32_t address, uint8_t* data, uint32_t length);
void flash_read_data(uint32_t address, uint8_t* buffer, uint32_t length);


int main(){
    *(volatile uint32_t*)0x40021018 |= (1<<4);
    *(volatile uint32_t*)0x40011004 &= ~(0xF<<20);
    *(volatile uint32_t*)0x40011004 |= (0x3<<20);

    uart_init();
    uart_send_string("STM32 booted\n");
    spi_init();

    // Test JEDEC
    spi_nss_low();
    spi_transmit(0x9F);
    uint8_t b1 = spi_receive();
    uint8_t b2 = spi_receive();
    uint8_t b3 = spi_receive();
    spi_nss_high();
    uart_send_byte(b1);
    uart_send_byte(b2);
    uart_send_byte(b3);

    // Test Write Enable
    flash_write_enable();
    spi_nss_low();
    spi_transmit(0x05);
    uint8_t status = spi_receive();
    spi_nss_high();
    uart_send_byte(status);

        // Erase sector 0
    flash_sector_erase(0x000000);

    // Write test byte
    uint8_t write_buf[1] = {0xAB};
    flash_page_program(0x000000, write_buf, 1);

    // Read it back
    uint8_t read_buf[1] = {0};
    flash_read_data(0x000000, read_buf, 1);

    uart_send_byte(read_buf[0]);

    while(true){
        *(volatile uint32_t*)0x4001100C |= (1<<13);
        for(volatile int i = 0; i < 1000000; i++);
        *(volatile uint32_t*)0x4001100C &= ~(1<<13);
        for(volatile int i = 0; i < 1000000; i++);
    }
    return 0;
}