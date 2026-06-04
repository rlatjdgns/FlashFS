#pragma once
#include <stdint.h>

void spi_init();
void spi_transmit(uint8_t data);
uint8_t spi_receive();
void spi_nss_low();
void spi_nss_high();
void spi_bitbang_init();
uint8_t spi_bitbang(uint8_t out);