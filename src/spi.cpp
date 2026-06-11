#include <stdint.h>

void uart_send_byte(uint8_t data);
void uart_send_string(const char* s);

#define SPI_SR (*(volatile uint32_t*)0x40013008)

// One-shot latch so a dead SPI bus reports once instead of flooding the UART
// (flash_read_data alone would otherwise print thousands of times).
static int spi_to_reported = 0;

// Spin on (cond) but bail after a bounded count so a stalled/absent SPI flash
// turns into a recoverable garbage read instead of hanging the CPU forever.
// On the first timeout it announces a TXE/RXNE stall (SPI peripheral level).
#define SPI_WAIT(cond) do {                                                \
    uint32_t _to = 1000000;                                                \
    while(!(cond)){                                                        \
        if(--_to == 0){                                                    \
            if(!spi_to_reported){ spi_to_reported = 1;                     \
                uart_send_string("SPI:BYTE-TIMEOUT (TXE/RXNE stuck)\n"); } \
            break;                                                         \
        }                                                                  \
    }                                                                      \
} while(0)

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
    if(SPI_SR & (1<<0)){
        (void)*(volatile uint32_t*)0x4001300C;
    }
    //Wait until TX becomes empty and write to DR to start transmission
    SPI_WAIT(SPI_SR & (1<<1));                 // TXE
    *(volatile uint32_t*)0x4001300C = data;

    //Wait until RXNE is full and discard the garbage bits
    SPI_WAIT(SPI_SR & (1<<0));                 // RXNE
    (void)*(volatile uint32_t*)0x4001300C;
}

uint8_t spi_receive(){
    //Wait for TXE to be empty and write dummy to DR
    SPI_WAIT(SPI_SR & (1<<1));                 // TXE
    *(volatile uint32_t*)0x4001300C = 0x00;

    //Wait for RXNE to fill and return byte that came back on MISO
    SPI_WAIT(SPI_SR & (1<<0));                 // RXNE
    return *(volatile uint32_t*)0x4001300C;
}

void spi_nss_low(){
    *(volatile uint32_t*)0x4001080C &= ~(1<<4);
}

void spi_nss_high(){
    *(volatile uint32_t*)0x4001080C |= (1<<4);
}