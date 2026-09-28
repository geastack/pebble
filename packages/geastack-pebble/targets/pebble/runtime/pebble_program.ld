/* SPDX-License-Identifier: Apache-2.0
 *
 * The Gea program blob: one position-independent image, linked at 0 and
 * relocated by the shell (pebble_app.c) to wherever its heap put it. The
 * program header comes first so the shell can find the entry table without a
 * symbol table. Everything the image needs at run time -- code, constants,
 * data, the GOT, constructor tables -- is laid out contiguously and
 * followed by .bss, which the shell zeroes.
 */
ENTRY(gea_pebble_program)

SECTIONS
{
  . = 0;
  .gea_program : { KEEP(*(.gea_program)) }

  .text : {
    *(.text .text.* .gnu.linkonce.t.*)
    *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
    _etext = .;
  }

  .rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) }


  .preinit_array : {
    __preinit_array_start = .;
    KEEP(*(.preinit_array))
    __preinit_array_end = .;
  }
  .init_array : {
    __init_array_start = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  }
  .fini_array : { KEEP(*(.fini_array)) KEEP(*(SORT(.fini_array.*))) }

  .dynamic : { *(.dynamic) }
  .got : { *(.got.plt) *(.igot.plt) *(.got) *(.igot) }

  .data : { *(.data .data.* .gnu.linkonce.d.*) }

  . = ALIGN(8);
  __gea_image_end = .;

  .bss (NOLOAD) : {
    *(.bss .bss.* .gnu.linkonce.b.*)
    *(COMMON)
    . = ALIGN(8);
  }
  __gea_memory_end = .;

  /* Read by the build, which turns it into the blob's relocation list; not
   * part of the loaded image. */
  .rel.dyn : { *(.rel.dyn) *(.rel.*) }

  /DISCARD/ : {
    *(.interp) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash)
    *(.note.*) *(.comment) *(.ARM.attributes)
    /* Unwind tables: only the unwinder reads them, and pebble_support.cpp
     * replaces it with stubs that end the app at the throw. */
    *(.ARM.extab* .gnu.linkonce.armextab.*) *(.ARM.exidx* .gnu.linkonce.armexidx.*)
  }
}
