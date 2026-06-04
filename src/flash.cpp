#include <stdint.h>
#include "flash.h"
#include "spi.h"

void spi_transmit(uint8_t data);
uint8_t spi_receive();
void spi_nss_high();
void spi_nss_low();
void uart_send_byte(uint8_t data);
void uart_send_string(const char* s);

void flash_read_jedec_id(){
    spi_nss_low();
    spi_transmit(0x9F);
    uint8_t b1 = spi_receive();
    uint8_t b2 = spi_receive();
    uint8_t b3 = spi_receive();
    spi_nss_high();
    uart_send_byte(b1);
    uart_send_byte(b2);
    uart_send_byte(b3);
}

void flash_write_enable(){
    spi_nss_low();
    spi_transmit(0x06);
    spi_nss_high();
}

void flash_wait_busy(){
    uint8_t status;
    do{
        spi_nss_low();
        spi_transmit(0x05);
        status = spi_receive();
        spi_nss_high();
    }while(status & 0x01);
}

void flash_sector_erase(uint32_t address){
    flash_write_enable();
    spi_nss_low();
    spi_transmit(0x20);
    spi_transmit((address >> 16) & 0xFF);
    spi_transmit((address >> 8) & 0xFF);
    spi_transmit((address >> 0) & 0xFF);
    spi_nss_high();
    flash_wait_busy();
}

void flash_page_program(uint32_t address, uint8_t* data, uint32_t length){
    flash_write_enable();
    spi_nss_low();
    spi_transmit(0x02);
    spi_transmit((address >> 16) & 0xFF);
    spi_transmit((address >> 8) & 0xFF);
    spi_transmit((address >> 0) & 0xFF);
    for(uint32_t i = 0; i < length; i++){
        spi_transmit(*data);
        data++;
    }
    spi_nss_high();
    flash_wait_busy();
}

void flash_read_data(uint32_t address, uint8_t* buffer, uint32_t length){
    spi_nss_low();
    spi_transmit(0x03);
    spi_transmit((address >> 16) & 0xFF);
    spi_transmit((address >> 8) & 0xFF);
    spi_transmit((address >> 0) & 0xFF);
    for(uint32_t i = 0; i < length; i++){
        buffer[i] = spi_receive();
    }
    spi_nss_high();
}