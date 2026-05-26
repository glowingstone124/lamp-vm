/* LampVM legacy flat boot image format (boot.bin)
 *
 * File layout:
 *   [24-byte header: 6 x u32 little-endian]
 *   [text bytes]
 *   [data bytes]
 *
 * Header fields:
 *   TEXT_BASE, TEXT_SIZE, DATA_BASE, DATA_SIZE, BSS_BASE, BSS_SIZE
 */

OUTPUT_FORMAT(binary)
ENTRY(_start)

  /* VM default program load base (see src/vm.h: PROGRAM_BASE). */
TEXT_BASE = 0x0000201C;
HDR_BASE = TEXT_BASE - 24;

SECTIONS {
  /*
   * Place header right before TEXT_BASE in link address space.
   * For binary output, this keeps file layout compact:
   *   [header][text][data]
   * while symbol/runtime addresses still start at TEXT_BASE.
   */
  . = HDR_BASE;

  .flat_header : {
    LONG(TEXT_BASE);
    LONG(ALIGN(SIZEOF(.text_payload), 8));
    LONG(TEXT_BASE + ALIGN(SIZEOF(.text_payload), 8));
    LONG(SIZEOF(.data_payload));
    LONG(TEXT_BASE + ALIGN(SIZEOF(.text_payload), 8) + SIZEOF(.data_payload));
    LONG(SIZEOF(.bss_payload));
  }

  .text_payload TEXT_BASE : {
    KEEP(*(.text.startup))
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
    . = ALIGN(8);
    BYTE(0);
    . = ALIGN(8);
  }

  .data_payload : {
    *(.data .data.*)
    *(.sdata .sdata.*)
  }

  .bss_payload (NOLOAD) : {
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
  }

  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.eh_frame_hdr*)
    *(.llvm_addrsig)
  }
}
