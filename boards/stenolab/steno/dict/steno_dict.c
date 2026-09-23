/*
 * Locate the compiled dictionary in internal flash and open it.
 *
 * On nRF52840 the internal flash is memory mapped at 0x00000000, so a
 * fixed-partition's devicetree offset IS its absolute address and the image
 * can be read in place with no copy and no flash driver.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "sdic.h"

LOG_MODULE_REGISTER(steno_dict, CONFIG_STENO_DICT_LOG_LEVEL);

#define DICT_NODE DT_NODELABEL(dict_partition)
BUILD_ASSERT(DT_NODE_EXISTS(DICT_NODE), "no dict_partition in devicetree");

#define DICT_OFF  DT_REG_ADDR(DICT_NODE)
#define DICT_SIZE DT_REG_SIZE(DICT_NODE)

/* The flash controller this partition lives on must be memory mapped at 0. */
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(flash0)) == 0,
	     "flash0 is not mapped at 0; dictionary addressing assumes it is");

static sdic dict;
static bool dict_ready;

int steno_dict_init(void)
{
	const void *base = (const void *)(uintptr_t)DICT_OFF;
	int rc = sdic_open(&dict, base, DICT_SIZE);

	if (rc != SDIC_OK) {
		/*
		 * Almost always "no image flashed yet". Do not fail the boot:
		 * the keyboard should still enumerate and report battery so
		 * the user can see it is alive and flash a dictionary.
		 */
		LOG_ERR("no usable dictionary at 0x%08x (rc=%d)",
			(unsigned)DICT_OFF, rc);
		dict_ready = false;
		return rc;
	}
	dict_ready = true;
	LOG_INF("dictionary: %u entries, %u values, %u B of %u B partition",
		dict.hdr->n_entries, dict.hdr->n_values,
		dict.hdr->image_size, (unsigned)DICT_SIZE);
	return 0;
}

bool steno_dict_available(void)
{
	return dict_ready;
}

int steno_dict_lookup(const uint32_t *strokes, unsigned n,
		      char *out, size_t outsz)
{
	if (!dict_ready)
		return SDIC_E_NOTFOUND;
	return sdic_lookup(&dict, strokes, n, out, outsz);
}

static int steno_dict_sys_init(void)
{
	steno_dict_init();
	return 0;
}
SYS_INIT(steno_dict_sys_init, APPLICATION, 90);
