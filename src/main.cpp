#include <stdint.h>
#include "demo.h"
#include "fs.h"
#include "uart.h"
#include "bme280.h"

void spi_init();

static void uart_send_u32(uint32_t v){
    uart_send_byte((v >> 24) & 0xFF);
    uart_send_byte((v >> 16) & 0xFF);
    uart_send_byte((v >> 8)  & 0xFF);
    uart_send_byte((v >> 0)  & 0xFF);
}

int main(){
    *(volatile uint32_t*)0x40021018 |= (1<<4);
    *(volatile uint32_t*)0x40011004 &= ~(0xF<<20);
    *(volatile uint32_t*)0x40011004 |= (0x3<<20);

    uart_init();
    uart_send_string("STM32 booted\n");
    spi_init();
    fs_init();
    // One file per slot: file_id 0..MAX_FILES-1. Idempotent across reboots.
    char name[4];
    name[0] = 's';
    for(uint8_t i = 0; i < MAX_FILES; i++){
        name[1] = '0' + (i / 10);
        name[2] = '0' + (i % 10);
        name[3] = '\0';
        fs_create(name);
    }

    bme280_init();

    uint32_t seq = 0;
    
    while(true){
        int32_t T = bme280_read_temp();

        SensorReading reading;
        reading.seq = seq; 
        reading.temp = T; 
        fs_write(seq % MAX_FILES, (uint8_t*)&reading, sizeof(SensorReading));
        uart_send_u32(seq);
        uart_send_u32((uint32_t)T);
        if(seq > 0 && seq % 5 == 0){
            uart_send_string("READBACK:\n");
            for(uint8_t fid = 0; fid < MAX_FILES; fid++){
                SensorReading r;
                r.seq = 0;
                r.temp = 0;
                // fs_read returns 0 only when real data was copied; -1 for an
                // unwritten/missing slot or a CRC failure -> skip it.
                if(fs_read(fid, (uint8_t*)&r, sizeof(SensorReading)) == 0){
                    uart_send_u32(r.seq);
                    uart_send_u32((uint32_t)r.temp);
                }
            }
            uart_send_string("END\n");
        }
        for(volatile int i = 0; i < 10000000; i++);
        seq++;
    }
    return 0;
}