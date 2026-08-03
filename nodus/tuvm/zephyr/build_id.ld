/* SPIKE (#56 / D5): give .note.gnu.build-id an explicit home, and symbols to find it.
 *
 * This is an INPUT-section fragment, not a full output section: it is included via
 * zephyr_linker_sources(RODATA ...), which splices it INSIDE the existing rodata
 * output section. On this SoC rodata lives in DROM (mapped flash), so the note is
 * loadable and CPU-readable — which is the whole requirement. An unallocated note
 * lives only in the ELF and is stripped from the .bin, so the firmware could never
 * read it back.
 *
 * RODATA rather than the obvious ROM_SECTIONS, and that is the spike's first real
 * finding: Espressif SoCs do not use the generic RISC-V linker template. Only
 * zephyr/include/zephyr/arch/riscv/common/linker.ld includes snippets-rom-sections.ld;
 * soc/espressif/esp32c6/default.ld does not. It honours exactly four snippet hooks —
 * rwdata, data-sections, ram-sections and rodata (line 845). So
 * zephyr_linker_sources(ROM_SECTIONS ...) is accepted by CMake, generates its snippet
 * file, and is then SILENTLY DROPPED on this platform. The symptom is an orphan-section
 * warning and undefined bracketing symbols, not an error naming the cause.
 *
 * KEEP() because --gc-sections is in Zephyr's base linker flags and nothing references
 * the note's contents.
 */
. = ALIGN(4);
__build_id_note_start = .;
KEEP(*(.note.gnu.build-id))
__build_id_note_end = .;
. = ALIGN(4);
