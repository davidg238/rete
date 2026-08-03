/* SPIKE (#56 / D5): read this image's own GNU build-id and print it at boot.
 *
 * The probe this is half of: `readelf -n zephyr.elf` must show a note, the
 * flashed board must print the same 20 bytes, and the two must agree. #98 is
 * precedent that this platform's segment walk misclassifies sections and fails
 * SILENTLY, so the ELF containing a note proves nothing about the running image.
 *
 * Note layout is Elf32_Nhdr followed by a 4-byte-aligned name then the desc:
 *   u32 namesz; u32 descsz; u32 type; char name[namesz]; u8 desc[descsz];
 * For a GNU build-id: name = "GNU\0" (namesz 4), type = 3 (NT_GNU_BUILD_ID),
 * descsz = 20 for --build-id=sha1.
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <string.h>
#include <stdint.h>

extern const uint8_t __build_id_note_start[];
extern const uint8_t __build_id_note_end[];

#define NT_GNU_BUILD_ID 3

static int build_id_banner(void)
{
	const uint8_t *p = __build_id_note_start;
	size_t span = (size_t)(__build_id_note_end - __build_id_note_start);
	uint32_t namesz, descsz, type;

	/* Report the span unconditionally and FIRST. A zero span is the exact
	 * failure this probe exists to catch, and it must be visible as a
	 * printed fact rather than as the absence of a build-id line. */
	printk("build-id: note span=%u bytes\n", (unsigned)span);
	if (span < 16U) {
		printk("build-id: ABSENT — no note reached the image\n");
		return 0;
	}

	memcpy(&namesz, p + 0, sizeof(namesz));
	memcpy(&descsz, p + 4, sizeof(descsz));
	memcpy(&type, p + 8, sizeof(type));

	if (type != NT_GNU_BUILD_ID || descsz == 0U || descsz > 64U ||
	    (size_t)(12 + ((namesz + 3U) & ~3U) + descsz) > span) {
		printk("build-id: MALFORMED namesz=%u descsz=%u type=%u\n",
		       namesz, descsz, type);
		return 0;
	}

	const uint8_t *desc = p + 12 + ((namesz + 3U) & ~3U);

	printk("build-id: ");
	for (uint32_t i = 0; i < descsz; i++) {
		printk("%02x", desc[i]);
	}
	printk("\n");
	return 0;
}

SYS_INIT(build_id_banner, APPLICATION, 0);
