MEMORY
{
    FLASH(rx) : ORIGIN = 0x08000000, LENGTH = 64K
    RAM(rwx)  : ORIGIN = 0x20000000, LENGTH = 20K
}

SECTIONS
{
    .text :
    {
        *(.isr_vector)
        *(.text)
    } >FLASH

    .rodata :
    {
        *(.rodata)
    } >FLASH

    .data :
    {
        _data_start = .;
        *(.data)
        _data_end = .;
    } >RAM AT> FLASH
    _data_load = LOADADDR(.data);

    .bss :
    {
        _bss_start = .;
        *(.bss)
        _bss_end = .;
    } >RAM
}
