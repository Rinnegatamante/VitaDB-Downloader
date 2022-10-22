/*
 * This file is part of VitaDB Downloader
 * Copyright 2022 Rinnegatamante
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
 
#ifndef _ADRENALINE_H_
#define _ADRENALINE_H_

#define ADRENALINE_CFG_MAGIC_1 0x31483943
#define ADRENALINE_CFG_MAGIC_2 0x334F4E33

enum MemoryStickLocations {
	MEMORY_STICK_LOCATION_UX0,
	MEMORY_STICK_LOCATION_UR0,
	MEMORY_STICK_LOCATION_IMC0,
	MEMORY_STICK_LOCATION_XMC0,
	MEMORY_STICK_LOCATION_UMA0,
};

typedef struct {
	int magic[2];
	int graphics_filtering;
	int no_smooth_graphics;
	int flux_mode;
	int screen_size;
	int ms_location;
	int use_ds3_ds4;
	int screen_mode;
	int skip_logo;
	float psp_screen_scale_x;
	float psp_screen_scale_y;
	float ps1_screen_scale_x;
	float ps1_screen_scale_y;
	int usbdevice;
} AdrenalineConfig;

#endif
