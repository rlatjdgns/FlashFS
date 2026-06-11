#include <stdint.h>
extern uint32_t _data_start;
extern uint32_t _data_end;
extern uint32_t _data_load;
extern uint32_t _bss_start;
extern uint32_t _bss_end;

typedef void (*handler_t)(void);
int main(void);
void reset_handler(void);

//Default trap for any unhandled exception — spin so a fault is observable instead of corrupting flow
static void default_handler(void){ while(1); }

__attribute__((section(".isr_vector")))
const handler_t vector_table[] = {
    (handler_t)(0x20000000 + 20 * 1024),  // 0: Initial stack pointer
    reset_handler,                         // 1: Reset
    default_handler,                       // 2: NMI
    default_handler,                       // 3: HardFault
    default_handler,                       // 4: MemManage
    default_handler,                       // 5: BusFault
    default_handler,                       // 6: UsageFault
};

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


