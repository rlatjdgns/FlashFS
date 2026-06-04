#include <stdint.h>

void uart_send_byte(uint8_t data);
void uart_send_string(const char* s);

void spi_init(){
    *(volatile uint32_t*)0x40021018 |= (1<<12)|(1<<2);
    
    *(volatile uint32_t*)0x40010800 &= ~(0xF<<16);
    *(volatile uint32_t*)0x40010800 |= (0x3<<16);
    
    *(volatile uint32_t*)0x40010800 &= ~(0xF<<20);
    *(volatile uint32_t*)0x40010800 |= (0xB<<20);
    
    *(volatile uint32_t*)0x40010800 &= ~(0xF<<24);
    *(volatile uint32_t*)0x40010800 |= (0x4<<24);
    
    *(volatile uint32_t*)0x40010800 &= ~(0xF<<28);
    *(volatile uint32_t*)0x40010800 |= (0xB<<28);
    
    *(volatile uint32_t*)0x40013000 = (1<<2)|(0x7<<3)|(1<<8)|(1<<9)|(1<<6);
    
    *(volatile uint32_t*)0x4001080C |= (1<<4);
}

// void spi_transmit(uint8_t data){
//     while(!(*(volatile uint32_t*)0x40013008 & (1<<1)));
//     *(volatile uint32_t*)0x4001300C = data;
// }

// uint8_t spi_receive(){
//     spi_transmit(0x00);
//     while(!(*(volatile uint32_t*)0x40013008 & (1<<0)));
//     uint8_t data = *(volatile uint32_t*)0x4001300C;
//     return data;
// }

void spi_transmit(uint8_t data){
    // clear any stale RXNE first
    if(*(volatile uint32_t*)0x40013008 & (1<<0)){
        (void)*(volatile uint32_t*)0x4001300C;
    }
    while(!(*(volatile uint32_t*)0x40013008 & (1<<1)));
    *(volatile uint32_t*)0x4001300C = data;
    while(!(*(volatile uint32_t*)0x40013008 & (1<<0)));
    (void)*(volatile uint32_t*)0x4001300C;  // drain byte received during transmit
}

uint8_t spi_receive(){
    while(!(*(volatile uint32_t*)0x40013008 & (1<<1)));
    *(volatile uint32_t*)0x4001300C = 0x00;
    while(!(*(volatile uint32_t*)0x40013008 & (1<<0)));
    return *(volatile uint32_t*)0x4001300C;
}

void spi_nss_low(){
    *(volatile uint32_t*)0x4001080C &= ~(1<<4);
}

void spi_nss_high(){
    *(volatile uint32_t*)0x4001080C |= (1<<4);
}

void spi_bitbang_init(){
    *(volatile uint32_t*)0x40021018 |= (1<<2);

    *(volatile uint32_t*)0x40010800 &= ~(0xF<<16);
    *(volatile uint32_t*)0x40010800 |= (0x3<<16);

    *(volatile uint32_t*)0x40010800 &= ~(0xF<<20);
    *(volatile uint32_t*)0x40010800 |= (0x3<<20);

    *(volatile uint32_t*)0x40010800 &= ~(0xF<<24);
    *(volatile uint32_t*)0x40010800 |= (0x4<<24);

    *(volatile uint32_t*)0x40010800 &= ~(0xF<<28);
    *(volatile uint32_t*)0x40010800 |= (0x3<<28);

    *(volatile uint32_t*)0x4001080C &= ~(1<<5);
    *(volatile uint32_t*)0x4001080C |= (1<<4);
}

uint8_t spi_bitbang(uint8_t out){
    uint8_t in = 0;
    for(int i = 7; i >= 0; i--){
        if(out & (1<<i))
            *(volatile uint32_t*)0x4001080C |= (1<<7);
        else
            *(volatile uint32_t*)0x4001080C &= ~(1<<7);

        *(volatile uint32_t*)0x4001080C |= (1<<5);
        if(*(volatile uint32_t*)0x40010808 & (1<<6))
            in |= (1<<i);
        for(volatile int d = 0; d < 10; d++);
        *(volatile uint32_t*)0x4001080C &= ~(1<<5);
        for(volatile int d = 0; d < 10; d++);
    }
    return in;
}

void spi_debug_transaction(uint8_t out){
    uart_send_string("CS SCK MOSI MISO\n");
    for(int i = 7; i >= 0; i--){
        if(out & (1<<i))
            *(volatile uint32_t*)0x4001080C |= (1<<7);
        else
            *(volatile uint32_t*)0x4001080C &= ~(1<<7);

        uint8_t cs   = (*(volatile uint32_t*)0x40010808 >> 4) & 1;
        uint8_t sck  = (*(volatile uint32_t*)0x40010808 >> 5) & 1;
        uint8_t mosi = (*(volatile uint32_t*)0x40010808 >> 7) & 1;
        uint8_t miso = (*(volatile uint32_t*)0x40010808 >> 6) & 1;

        uart_send_byte('0' + cs);
        uart_send_byte(' ');
        uart_send_byte('0' + sck);
        uart_send_byte(' ');
        uart_send_byte('0' + mosi);
        uart_send_byte(' ');
        uart_send_byte('0' + miso);
        uart_send_byte('\n');

        *(volatile uint32_t*)0x4001080C |= (1<<5);
        for(volatile int d = 0; d < 100; d++);
        *(volatile uint32_t*)0x4001080C &= ~(1<<5);
        for(volatile int d = 0; d < 100; d++);
    }
}