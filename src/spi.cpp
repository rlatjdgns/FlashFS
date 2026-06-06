#include <stdint.h>

void uart_send_byte(uint8_t data);
void uart_send_string(const char* s);

void spi_init(){
    //SPI1(12), GPIO(2) clock enable in RCC 
    *(volatile uint32_t*)0x40021018 |= (1<<12)|(1<<2);
    
    //NSS configuration 
    *(volatile uint32_t*)0x40010800 &= ~(0xF<<16);
    *(volatile uint32_t*)0x40010800 |= (0x3<<16);
    
    //PA5 (SCK) configuration  
    *(volatile uint32_t*)0x40010800 &= ~(0xF<<20);
    *(volatile uint32_t*)0x40010800 |= (0xB<<20);
    
    //PA6(MISO) configuration 
    *(volatile uint32_t*)0x40010800 &= ~(0xF<<24);
    *(volatile uint32_t*)0x40010800 |= (0x4<<24);
    
    //PA7 (MOSI) configuration 
    *(volatile uint32_t*)0x40010800 &= ~(0xF<<28);
    *(volatile uint32_t*)0x40010800 |= (0xB<<28);
    
    //SPI1 enable in CR1 
    *(volatile uint32_t*)0x40013000 = (1<<2)|(0x7<<3)|(1<<8)|(1<<9)|(1<<6);
    
    //ODR configuration 
    *(volatile uint32_t*)0x4001080C |= (1<<4);
}

void spi_transmit(uint8_t data){
    //Check if empty if not drain the bits 
    if(*(volatile uint32_t*)0x40013008 & (1<<0)){
        (void)*(volatile uint32_t*)0x4001300C;
    }
    //Wait until TX becomes empty and write to DR to start transmission 
    while(!(*(volatile uint32_t*)0x40013008 & (1<<1)));
    *(volatile uint32_t*)0x4001300C = data;

    //Wait until RXNE is full and discard the garbage bits 
    while(!(*(volatile uint32_t*)0x40013008 & (1<<0)));
    (void)*(volatile uint32_t*)0x4001300C;  
}

uint8_t spi_receive(){
    //Wait for TXE to be empty and write dummy to DR 
    while(!(*(volatile uint32_t*)0x40013008 & (1<<1)));
    *(volatile uint32_t*)0x4001300C = 0x00;

    //Wait for RXNE to fill and return byte that came back on MISO 
    while(!(*(volatile uint32_t*)0x40013008 & (1<<0)));
    return *(volatile uint32_t*)0x4001300C;
}

void spi_nss_low(){
    *(volatile uint32_t*)0x4001080C &= ~(1<<4);
}

void spi_nss_high(){
    *(volatile uint32_t*)0x4001080C |= (1<<4);
}