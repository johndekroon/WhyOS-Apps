// SPDX-License-Identifier: MIT
/*
 * lora_presets.h — the LoRa radio settings (docs/LORA.md, docs/SETTINGS.md):
 * MeshCore's suggested presets and the settings that hold the choice.
 *
 *   lora.radio  "FREQ_KHZ BW_HZ SF CR", e.g. "869618 62500 7 5"
 *   lora.power  dBm
 *
 * Settings > Connection > LoRa sets them; S60lora hands them to the mesh
 * daemon (meshcored -r, -x) when it starts it; meshchat names the preset.
 */
#ifndef LORA_PRESETS_H
#define LORA_PRESETS_H

#include <stdint.h>
#include <stdlib.h>

/* MeshCore's suggested presets (api.meshcore.nz, 2026-10-17), those the
 * badge's Ra-01SH can do (803-930 MHz). */
static const struct lora_preset {
	const char *name;
	uint32_t freq_khz, bw_hz;
	uint8_t sf, cr;
} lora_presets[] = {
	{ "Netherlands", 869618, 62500, 7, 5 },
	{ "EU/UK (Narrow)", 869618, 62500, 8, 8 },
	{ "EU/UK (Deprecated)", 869525, 250000, 11, 5 },
	{ "Czech Republic", 869432, 62500, 7, 5 },
	{ "Portugal 868", 869618, 62500, 7, 6 },
	{ "USA", 910525, 62500, 7, 5 },
	{ "Australia", 915800, 250000, 10, 5 },
};
#define LORA_N_PRESETS	(int)(sizeof(lora_presets) / sizeof(lora_presets[0]))

#define LORA_RADIO_DEFAULT	"869618 62500 7 5"	/* Netherlands */
#define LORA_POWER_DEFAULT	14
#define LORA_POWER_MIN		2
#define LORA_POWER_MAX		22		/* the SX1262's */

struct lora_radio {
	uint32_t freq_khz, bw_hz;
	int sf, cr;
};

/* "869618 62500 7 5"; 0, or -1 when it is not four sane numbers */
static inline int lora_radio_parse(const char *s, struct lora_radio *r)
{
	char *e;

	r->freq_khz = (uint32_t)strtoul(s, &e, 10);
	r->bw_hz = (uint32_t)strtoul(e, &e, 10);
	r->sf = (int)strtol(e, &e, 10);
	r->cr = (int)strtol(e, &e, 10);
	return r->freq_khz >= 150000 && r->freq_khz <= 2500000 && r->bw_hz >= 7000 &&
	       r->bw_hz <= 500000 && r->sf >= 5 && r->sf <= 12 && r->cr >= 5 && r->cr <= 8 ? 0 : -1;
}

/* the preset with these settings, or -1 */
static inline int lora_preset_of(uint32_t freq_khz, uint32_t bw_hz, int sf, int cr)
{
	for (int i = 0; i < LORA_N_PRESETS; i++)
		if (lora_presets[i].freq_khz == freq_khz && lora_presets[i].bw_hz == bw_hz &&
		    lora_presets[i].sf == sf && lora_presets[i].cr == cr)
			return i;
	return -1;
}

#endif
