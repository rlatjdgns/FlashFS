#include <stdint.h>

void uart_init(){
    //Enable UART1, GPIOA, and AFIO clock in RCC APB2ENR register 
    *(volatile uint32_t*)0x40021018 |= (1<<14)|(1<<2)|(1<<0);

    //GPIO configuration for PA9 in data register
    *(volatile uint32_t*)0x40010804 &= ~(0XF<<4);
    *(volatile uint32_t*)0x40010804 |= (0xB<<4);

    //GPIO configuration for PA10 in data register
    *(volatile uint32_t*)0x40010804 &= ~(0XF<<8);
    *(volatile uint32_t*)0x40010804 |= (0x4<<8);

    //Write BAUDRATE in Baudrate register
    *(volatile uint32_t*)0x40013808 = 69;
    
    //Enable UART and transmitter for USART1 CR 1 
    *(volatile uint32_t*)0x4001380C |= (1<<13)|(1<<3);
}

//Accept new byte for DR when bit7 of SR is 1 
void uart_send_byte(uint8_t data){
    while(!(*(volatile uint32_t*)0x40013800 & (1<<7)));
    *(volatile uint32_t*)0x40013804 = data;
}

//Send byte for string 
void uart_send_string(const char* s){
    while(*s != '\0'){
        uart_send_byte(*s);
        s++;
    }
}

