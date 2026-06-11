#include <stdint.h>
#include "i2c.h"
#include "bme280.h"

void uart_send_string(const char* s);

// Flip I2C_DEBUG to 1 to print which transaction stage timed out over UART.
// e.g. "I2C:to R-ADDRR" = the read-address phase never got an ACK.
#define I2C_DEBUG 0
#if I2C_DEBUG
  #define I2C_DBG(tag) uart_send_string("I2C:to " tag "\n")
#else
  #define I2C_DBG(tag) ((void)0)
#endif

// Spin on (cond) but bail out after a bounded number of iterations so a stuck
// bus / missing flag can never hang the CPU forever. On timeout it prints the
// stage `tag` (when I2C_DEBUG) then runs `on_timeout` to recover.
#define I2C_WAIT(tag, cond, on_timeout) do {                          \
    uint32_t _to = 100000;                                            \
    while(!(cond)){ if(--_to == 0){ I2C_DBG(tag); on_timeout; } }     \
} while(0)

#define I2C_SR1   (*(volatile uint32_t*)0x40005414)
#define I2C_CR1   (*(volatile uint32_t*)0x40005400)

void i2c_init(){
    // I2C1 on APB1 for RCC
    *(volatile uint32_t*)0x4002101C |= (1<<21);
    // GPIOB on APB2 for RCC 
    *(volatile uint32_t*)0x40021018 |= (1<<3);   

    //GPIO PB6 & PB 7 configuration
    *(volatile uint32_t*)0x40010C00 &= ~(0xF<<24);
    *(volatile uint32_t*)0x40010C00 |= (0xF<<24);

    *(volatile uint32_t*)0x40010C00 &= ~(0xF<<28);
    *(volatile uint32_t*)0x40010C00 |= (0xF<<28);

    //I2C clock speed configuraiton
    //Freq
    *(volatile uint32_t*)0x40005404 &= ~(0x3F<<0); 
    *(volatile uint32_t*)0x40005404 |= (0x8<<0);
    //CCR
    *(volatile uint32_t*)0x4000541C &= ~(0xFFF<<0);
    *(volatile uint32_t*)0x4000541C |= (0x28<<0);
    //TRISE  
    *(volatile uint32_t*)0x40005420 &= ~(0x3F<<0);
    *(volatile uint32_t*)0x40005420 |= (0x9<<0);

    //Enable I2C
    *(volatile uint32_t*)0x40005400 |= (0x1<<0);
}

// --- FIX: wait for the bus to go idle (BUSY=0) before issuing START. Issuing a
// START while the previous transaction's STOP is still completing makes the master
// drop off during the address phase — observed as R-ADDRW timeouts with
// SR1=0000/SR2=0000. Bounded so a genuinely stuck bus can't hang the CPU. ---
static void i2c_wait_idle(){
    uint32_t to = 100000;
    while((*(volatile uint32_t*)0x40005418) & (1<<1)){   // SR2 BUSY
        if(--to == 0){ return; }
    }
}

void i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t data){
    //Start
    i2c_wait_idle();
    I2C_CR1 |= (1<<8);
    I2C_WAIT("W-SB", I2C_SR1 & (1<<0), { I2C_CR1 |= (1<<9); return; });   // SB

    //Send Address on DR
    *(volatile uint32_t*)0x40005410 = (addr << 1) | 0;
    I2C_WAIT("W-ADDR", I2C_SR1 & (1<<1), { I2C_CR1 |= (1<<9); return; }); // ADDR (NACK = device absent)
    (void)*(volatile uint32_t*)0x40005418;

    //Send Register on DR
    *(volatile uint32_t*)0x40005410 = reg;

    //Wait for TXE, Send data on DR, wait for BTF
    I2C_WAIT("W-TXE", I2C_SR1 & (1<<7), { I2C_CR1 |= (1<<9); return; });  // TXE
    *(volatile uint32_t*)0x40005410 = data;
    I2C_WAIT("W-BTF", I2C_SR1 & (1<<2), { I2C_CR1 |= (1<<9); return; });  // BTF

    //Stop
    I2C_CR1 |= (1<<9);
}

uint8_t i2c_read_reg(uint8_t addr, uint8_t reg){
    //Start
    i2c_wait_idle();
    I2C_CR1 |= (1<<8);
    I2C_WAIT("R-SB", I2C_SR1 & (1<<0), { I2C_CR1 |= (1<<9); return 0; });    // SB

    //Addr+W
    *(volatile uint32_t*)0x40005410 = (addr << 1) | 0;
    I2C_WAIT("R-ADDRW", I2C_SR1 & (1<<1), { I2C_CR1 |= (1<<9); return 0; }); // ADDR (NACK = device absent)
    (void)*(volatile uint32_t*)0x40005418;

    *(volatile uint32_t*)0x40005410 = reg;
    I2C_WAIT("R-TXE", I2C_SR1 & (1<<7), { I2C_CR1 |= (1<<9); return 0; });   // TXE

    //Restart
    I2C_CR1 |= (1<<8);
    I2C_WAIT("R-SB2", I2C_SR1 & (1<<0), { I2C_CR1 |= (1<<9); return 0; });   // SB

    //Addr+R
    *(volatile uint32_t*)0x40005410 = (addr << 1) | 1;
    I2C_WAIT("R-ADDRR", I2C_SR1 & (1<<1), { I2C_CR1 |= (1<<9); return 0; }); // ADDR

    //Single-byte receive (RM0008 EV6_1): clear ACK and set STOP *before* clearing ADDR
    *(volatile uint32_t*)0x40005400 &= ~(1<<10);          // 1. Clear ACK
    (void)*(volatile uint32_t*)0x40005418;                // 2. Clear ADDR (read SR2)
    I2C_CR1 |= (1<<9);                                    // 3. Set STOP

    I2C_WAIT("R-RXNE", I2C_SR1 & (1<<6), { return 0; });  // 4. Wait RxNE (STOP already set)
    uint8_t data = *(volatile uint32_t*)0x40005410;       // 5. Read DR

    return data;
}

//BME280 temperature calibration, read once in bme280_init() and reused per read
static uint16_t dig_T1;
static int16_t dig_T2;
static int16_t dig_T3;

void bme280_init(){
    i2c_init();
    i2c_write_reg(0x76, 0xF4, 0x27);
    for(volatile int i = 0; i < 1000000; i++);

    uint8_t lsb = i2c_read_reg(0x76, 0x88);
    uint8_t msb = i2c_read_reg(0x76, 0x89);
    dig_T1 = (msb << 8) | lsb;

    uint8_t lsb2 = i2c_read_reg(0x76, 0x8A);
    uint8_t msb2 = i2c_read_reg(0x76, 0x8B);
    dig_T2 = (msb2 <<8)|lsb2;

    uint8_t lsb3 = i2c_read_reg(0x76, 0x8C);
    uint8_t msb3 = i2c_read_reg(0x76, 0x8D);
    dig_T3 = (msb3 <<8)|lsb3;
}

int32_t bme280_read_temp(){
    uint32_t temp_msb = i2c_read_reg(0x76, 0xFA);
    uint32_t temp_lsb = i2c_read_reg(0x76, 0xFB);
    uint32_t temp_xlsb = i2c_read_reg(0x76, 0xFC);
    int32_t adc_T = (temp_msb << 12) | (temp_lsb << 4) | (temp_xlsb >> 4);

    int32_t var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) * ((int32_t)dig_T3)) >> 14;
    int32_t t_fine = var1 + var2;
    int32_t T = (t_fine * 5 + 128) >> 8;
    return T;
}
