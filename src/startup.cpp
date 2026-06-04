#include <stdint.h>
extern uint32_t _data_start;
extern uint32_t _data_end;
extern uint32_t _data_load;
extern uint32_t _bss_start;
extern uint32_t _bss_end;

typedef void (*handler_t)(void);
int main(void);
void reset_handler(void);

__attribute__((section(".isr_vector")))
const handler_t vector_table[] = {(handler_t)(0x20000000 + 20 * 1024),reset_handler};

void reset_handler(){
    uint32_t *src = &_data_load;
    uint32_t *dst = &_data_start;
    uint32_t *bss = &_bss_start;

    while(dst<&_data_end){
        *dst = *src;
        dst++;
        src++; 
    }

    while(bss<&_bss_end){
        *bss = 0;
        bss++;
    }
    
    main();
}


