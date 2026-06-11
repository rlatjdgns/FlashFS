#pragma once
#include <stdint.h>

void uart_init();
void uart_send_byte(uint8_t data);
void uart_send_string(const char* s);
