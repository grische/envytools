/*
 * Copyright (C) 2018 Christian Schmidbauer
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include "bios.h"

int envy_bios_parse_bit_B(struct envy_bios *bios, struct envy_bios_bit_entry *bit) {
	struct envy_bios_biosdata *biosdata = &bios->biosdata;
	biosdata->bit = bit;

	uint16_t biosdata_size;
	if (bit->version == 1) {
		biosdata_size = sizeof(struct nv_bios_biosdata_v1);
	} else if (bit->version == 2) {
		biosdata_size = sizeof(struct nv_bios_biosdata_v2);
	} else {
		ENVY_BIOS_ERR("BIT 'B' table has unknown bios_version: %d\n", bit->version);
		return -EINVAL;
	}

	if (bit->t_len < biosdata_size) {
		ENVY_BIOS_ERR("BIT 'B' table v%d too short: 0x%04x < (expected) 0x%04x\n", bit->version, bit->t_len, biosdata_size);
		return -EINVAL;
	}

	if (bit->t_len != biosdata_size) {
		ENVY_BIOS_WARN("BIT 'B' table v%d has strange size: 0x%04x != (expected) 0x%04x\n", bit->version, bit->t_len, biosdata_size);
	}

	biosdata->biosdata_version = bit->version;

	// TODO this memcpy should be replaced by bios_u8, bios_u16, bios_u32
	memcpy(&biosdata->nv_biosdata, bios->data + bit->t_offset, biosdata_size);

	biosdata->valid = 1;

	// TODO: parse data_range_table properly

	return 0;
}


void envy_bios_print_biosdata_v2(struct envy_bios_biosdata *biosdata, FILE *out) {

	struct nv_bios_biosdata_v2 * nv_biosdata = &biosdata->nv_biosdata.biosdata_v2;

	fprintf(out, "INFO: BIOS Binary Version: 0x %02x.%02x.%02x.%02x\n", nv_biosdata->bios_version[0], nv_biosdata->bios_version[1], nv_biosdata->bios_version[2], nv_biosdata->bios_version[3]);
	fprintf(out, "INFO: BIOS OEM Version Number: 0x%02x\n", nv_biosdata->bios_oem_version);
	fprintf(out, "INFO: BIOS 0 Checksum inserted during the build: 0x%02x\n", nv_biosdata->bios_checksum);
	fprintf(out, "INFO: INT15 Callbacks during POST: 0x%02x\n", nv_biosdata->int15_post_callbacks);
	fprintf(out, "INFO: General INT15 Callbacks: 0x%02x\n", nv_biosdata->int15_system_callbacks);
	fprintf(out, "INFO: Number of frames to display SignOn Message: 0x%02x\n", nv_biosdata->signon_frame_count);
	fprintf(out, "INFO: Max Heads at POST 0x%02x\n", nv_biosdata->max_heads_at_post);
	fprintf(out, "INFO: Memory Size Report: 0x%02x\n", nv_biosdata->memory_size_report);
	fprintf(out, "INFO: Horizontal Scale Factor: 0x%02x\n", nv_biosdata->hscale_factor);
	fprintf(out, "INFO: Vertical Scale Factor: 0x%02x\n", nv_biosdata->vscale_factor);
	fprintf(out, "INFO: Data Range Table pointer: 0x%02x\n", nv_biosdata->data_range_table);
	fprintf(out, "INFO: Pointer to any ROMpacks: 0x%02x\n", nv_biosdata->rompacks_pointer);
	fprintf(out, "INFO: Applied Rompacks pointer: 0x%02x\n", nv_biosdata->applied_rompacks_ptr);
	fprintf(out, "INFO: Applied Rompacks' maximum number of stored indexes: 0x%02x\n", nv_biosdata->applied_rompacks_max);
	fprintf(out, "INFO: Number of applied run-time ROMpacks: 0x%02x\n", nv_biosdata->applied_rompacks_count);
	fprintf(out, "INFO: Module Map External 0 byte:: 0x%02x\n", nv_biosdata->module_map_external_0);
	fprintf(out, "INFO: Pointer to compression information: 0x%02x\n", nv_biosdata->compression_info_ptr);
}

void envy_bios_print_bit_B (struct envy_bios *bios, FILE *out, unsigned mask) {
	struct envy_bios_biosdata *biosdata = &bios->biosdata;
	if (!biosdata->bit || !(mask & ENVY_BIOS_PRINT_BIOSINFO)) {
		return;
	}

	if (!biosdata->valid) {
		fprintf(out, "Failed to parse BIT table 'B' v%d at 0x%04x\n\n", biosdata->bit->version, biosdata->bit->t_offset);
		return;
	}

	fprintf(out, "BIT table 'B' at 0x%x, version %i\n", biosdata->bit->offset, biosdata->bit->version);

	if(biosdata->biosdata_version == 2) {
		envy_bios_print_biosdata_v2(biosdata, out);
	}
	else {
		fprintf(out, "BIT table 'B' version %d has no readable printing support\n", biosdata->biosdata_version);
	};

	fprintf(out, "\n");
	envy_bios_dump_hex(bios, out, biosdata->bit->t_offset, biosdata->bit->t_len, mask);
	fprintf(out, "\n");
}
