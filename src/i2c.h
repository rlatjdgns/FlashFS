#pragma once
#include <stdint.h>

void i2c_init();
void i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t data);
uint8_t i2c_read_reg(uint8_t addr, uint8_t reg);
