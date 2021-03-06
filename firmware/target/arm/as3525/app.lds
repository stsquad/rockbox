#include "config.h"
#include "cpu.h"

ENTRY(start)

OUTPUT_FORMAT(elf32-littlearm)
OUTPUT_ARCH(arm)
STARTUP(target/arm/crt0.o)

#define PLUGINSIZE PLUGIN_BUFFER_SIZE
#define CODECSIZE CODEC_SIZE

#ifdef DEBUG
#define STUBOFFSET 0x10000
#else
#define STUBOFFSET 0
#endif


#define IRAMORIG IRAM_ORIG
#define DRAMORIG (DRAM_ORIG + STUBOFFSET)

/* End of the audio buffer, where the codec buffer starts */
#define ENDAUDIOADDR  (DRAMORIG + DRAMSIZE)


#ifdef AMS_LOWMEM
#define DRAMSIZE (DRAM_SIZE - STUBOFFSET - PLUGINSIZE - TTB_SIZE)
#define CODECORIG (IRAMORIG + (IRAM_SIZE - CODEC_SIZE))
#define IRAMSIZE (IRAM_SIZE - CODEC_SIZE)
#else
#define DRAMSIZE (DRAM_SIZE - STUBOFFSET - PLUGINSIZE - CODECSIZE - TTB_SIZE)
#define CODECORIG (ENDAUDIOADDR)
#define IRAMSIZE (0x20000)
#endif


/* Where the codec buffer ends, and the plugin buffer starts */
#ifdef AMS_LOWMEM
#define ENDADDR (ENDAUDIOADDR)
#else
#define ENDADDR (ENDAUDIOADDR + CODECSIZE)
#endif

MEMORY
{
#ifdef AMS_LOWMEM
    CODEC_IRAM : ORIGIN = CODECORIG, LENGTH = CODEC_SIZE
#endif
    IRAM : ORIGIN = IRAMORIG, LENGTH = IRAMSIZE
    DRAM : ORIGIN = DRAMORIG, LENGTH = DRAMSIZE
}

SECTIONS
{
    loadaddress = DRAM_ORIG;

    .vectors :
    {
        _vectors_start = .;
        *(.init.text)
    } > DRAM

    .text :
    {
        _loadaddress = .;
        _textstart = .;
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(0x4);
    } > DRAM

    .rodata :
    {
        *(.rodata*)
        . = ALIGN(0x4);
    } > DRAM

    .data :
    {
        *(.data*)
        . = ALIGN(0x4);
    } > DRAM

    /DISCARD/ :
    {
        *(.eh_frame)
    }

    .iram :
    {
        _iramstart = .;
        *(.icode)
        *(.irodata)
        *(.idata)
        . = ALIGN(0x4);
        _iramend = .;
    } > IRAM AT> DRAM

    .ibss (NOLOAD) :
    {
        _iedata = .;
        *(.qharray)
        *(.ibss)
        . = ALIGN(0x4);
        _iend = .;
    } > IRAM

    _iramcopy = LOADADDR(.iram);

    .stack (NOLOAD) :
    {
       *(.stack)
       stackbegin = .;
       . += 0x2000;
       stackend = .;
    } > DRAM

    .bss (NOLOAD) :
    {
       _edata = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(0x4);
       _end = .;
    } > DRAM

    .audiobuf (NOLOAD) :
    {
        . = ALIGN(4);
        _audiobuffer = .;
        audiobuffer = .;
    } > DRAM

    .audiobufend ENDAUDIOADDR (NOLOAD) :
    {
        audiobufend = .;
        _audiobufend = .;
    } > DRAM

    .codec CODECORIG (NOLOAD) :
    {
        codecbuf = .;
        _codecbuf = .;
#ifdef AMS_LOWMEM
    } > CODEC_IRAM
#else
    } > DRAM
#endif

    .plugin ENDADDR (NOLOAD) :
    {
        _pluginbuf = .;
        pluginbuf = .;
    }
}
