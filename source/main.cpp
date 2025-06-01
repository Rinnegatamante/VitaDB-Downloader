/*
 * This file is part of VitaDB Downloader
 * Copyright 2025 Rinnegatamante
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
 
#include <iostream>
#include <string>
#include <locale>
#include <codecvt>
#include <vitasdk.h>
#include <vitaGL.h>
#include <imgui_vita.h>
#include <imgui_internal.h>
#include <bzlib.h>
#include <stdio.h>
#include <malloc.h>
#include <taihen.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include "extractor.h"
#include "player.h"
#include "promoter.h"
#include "utils.h"
#include "dialogs.h"
#include "network.h"
#include "database.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define PREVIEW_PADDING 6
#define PREVIEW_PADDING_THEME 60
#define PREVIEW_HEIGHT (mode_idx == MODE_THEMES ? 159.0f : (mode_idx == MODE_VITA_HBS ? 128.0f : 80.0f))
#define PREVIEW_WIDTH  (mode_idx == MODE_THEMES ? 280.0f : (mode_idx == MODE_VITA_HBS ? 128.0f : 144.0f))

int _newlib_heap_size_user = 200 * 1024 * 1024;
int filter_idx = 0;
int cur_ss_idx;
int old_ss_idx = -1;
static char download_link[512];
char app_name_filter[128] = {0};
char boot_params[1024] = {};
SceUID audio_thd = -1;
void *audio_buffer = nullptr;
bool shuffle_themes = false;
int SCE_CTRL_CANCEL = SCE_CTRL_CROSS;
bool update_detected = false;

enum {
	KUBRIDGE_MISSING,
	KUBRIDGE_UX0,
	KUBRIDGE_UR0
};

const char *modes[] = {
	"Vita Homebrews",
	"PSP Homebrews",
	"Themes"
};
int mode_idx = 0;

static AppSelection *old_hovered = NULL;
ThemeSelection *themes = nullptr;
AppSelection *apps = nullptr;
AppSelection *psp_apps = nullptr;
AppSelection *to_download = nullptr;
static AppSelection *to_uninstall = nullptr;
TrophySelection *trophies = nullptr;


SceUID trophy_thd;
static int preview_width, preview_height, preview_x, preview_y;
GLuint preview_icon = 0, preview_shot = 0, previous_frame = 0, bg_image = 0, trp_icon = 0, empty_icon = 0;
int show_screenshots = 0; // 0 = off, 1 = download, 2 = show
int show_trailer = 0; // 0 = 0ff, 1 = prepare, 2 = show
int show_trophies = 0; // 0 = off, 1 = download, 2 = show
void LoadPreview(AppSelection *game) {
	if (old_hovered == game)
		return;
	old_hovered = game;
	
	char banner_path[256];
	if (mode_idx == MODE_THEMES) {
		ThemeSelection *g = (ThemeSelection *)game;
		sprintf(banner_path, "ux0:data/VitaDB/previews/%s.png", g->name);
	} else
		sprintf(banner_path, "ux0:data/VitaDB/icons/%c%c/%s", game->icon[0], game->icon[1], game->icon);
	uint8_t *icon_data = stbi_load(banner_path, &preview_width, &preview_height, NULL, 4);
	if (!preview_icon)
		glGenTextures(1, &preview_icon);
	glBindTexture(GL_TEXTURE_2D, preview_icon);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, preview_width, preview_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon_data);
	float scale = MIN(PREVIEW_WIDTH / (float)preview_width, PREVIEW_HEIGHT / (float)preview_height);
	preview_width = scale * (float)preview_width;
	preview_height = scale * (float)preview_height;
	preview_x = (PREVIEW_WIDTH - preview_width) / 2;
	preview_y = (PREVIEW_HEIGHT - preview_height) / 2;
	free(icon_data);
}

bool prepare_anti_burn_in() {
	uint8_t *scr_data = (uint8_t *)vglMalloc(960 * 544 * 4);
	glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, scr_data);
	if (!previous_frame)
		glGenTextures(1, &previous_frame);
	glBindTexture(GL_TEXTURE_2D, previous_frame);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 960, 544, 0, GL_RGBA, GL_UNSIGNED_BYTE, scr_data);
	vglFree(scr_data);
	return true;
}

void LoadScreenshot() {
	if (old_ss_idx == cur_ss_idx)
		return;
	bool is_incrementing = true;
	if (cur_ss_idx < 0) { 
		cur_ss_idx = 3;
		is_incrementing = false;
	} else if (cur_ss_idx > 3) {
		cur_ss_idx = 0;
	} else if (old_ss_idx > cur_ss_idx) {
		is_incrementing = false;
	}
	old_ss_idx = cur_ss_idx;
	
	char banner_path[256];
	sprintf(banner_path, "ux0:data/VitaDB/ss%d.png", cur_ss_idx);
	int w, h;
	uint8_t *shot_data = stbi_load(banner_path, &w, &h, NULL, 4);
	while (!shot_data) {
		cur_ss_idx += is_incrementing ? 1 : -1;
		if (cur_ss_idx < 0) { 
			cur_ss_idx = 3;
		} else if (cur_ss_idx > 3) {
			cur_ss_idx = 0;
		}
		sprintf(banner_path, "ux0:data/VitaDB/ss%d.png", cur_ss_idx);
		shot_data = stbi_load(banner_path, &w, &h, NULL, 4);
	}
	if (!preview_shot)
		glGenTextures(1, &preview_shot);
	glBindTexture(GL_TEXTURE_2D, preview_shot);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, shot_data);
	free(shot_data);
}

bool has_animated_bg = false;
void LoadBackground() {
	int w, h;
	
	SceIoStat st;
	if (sceIoGetstat("ux0:data/VitaDB/bg.mp4", &st) >= 0) {
		video_open("ux0:data/VitaDB/bg.mp4");
		has_animated_bg = true;
	} else {
		has_animated_bg = false;
		uint8_t *bg_data = stbi_load("ux0:data/VitaDB/bg.png", &w, &h, NULL, 4);
		if (bg_data) {
			glGenTextures(1, &bg_image);
			glBindTexture(GL_TEXTURE_2D, bg_image);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bg_data);
			free(bg_data);
		}
	}
}

volatile uint8_t early_stop_trp_icon_upload = 0;
int trophy_loader(unsigned int args, void *arg) {
	TrophySelection *t = trophies;
	while (t) {
		if (early_stop_trp_icon_upload)
			break;
		int w, h;
		char fname[256];
		sprintf(fname, "ux0:data/VitaDB/trophies/%s/%s", t->titleid, t->icon_name);
		GLuint res;
		glGenTextures(1, &res);
		uint8_t *icon_data = stbi_load(fname, &w, &h, NULL, 4);
		glTextureImage2D(res, GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon_data);
		free(icon_data);
		t->icon = res;
		t = t->next;
	}
	return sceKernelExitDeleteThread(0);
}

void PrepareTrophy(const char *tid, const char *name, int index, int count) {
	char fname[256], dl_url[256];
	sprintf(fname, "ux0:data/VitaDB/trophies/%s/%s", tid, name);
	SceIoStat st;
	if (sceIoGetstat(fname, &st) < 0) {
		sprintf(dl_url, "https://www.rinnegatamante.eu/vitadb/trophies/%s", name);
		download_file(dl_url, "Downloading trophy icon", false, index + 1, count);
		sceIoRename(TEMP_DOWNLOAD_NAME, fname);
	}
}

const char *sort_modes_str[] = {
	"Most Recent",
	"Oldest",
	"Most Downloaded",
	"Least Downloaded",
	"Alphabetical (A-Z)",
	"Alphabetical (Z-A)",
	"Smallest",
	"Largest"
};
const char *sort_modes_themes_str[] = {
	"Alphabetical (A-Z)",
	"Alphabetical (Z-A)"
};
const char *filter_modes[] = {
	"All Apps",
	"Original Games",
	"Game Ports",
	"Utilities",
	"Emulators",
	"Freeware Apps",
	"Not Installed Apps",
	"Outdated Apps",
	"Installed Apps",
	"Apps with Trophies",
};
const char *filter_themes_modes[] = {
	"All Themes",
	"Downloaded Themes",
	"Not Downloaded Themes"
};

int sort_idx = 0;
int old_sort_idx = -1;

bool filterApps(AppSelection *p) {
	if (filter_idx) {
		int filter_cat = filter_idx > 2 ? (filter_idx + 1) : filter_idx;
		if (filter_cat <= 5) {
			if (p->type[0] - '0' != filter_cat)
				return true;
		} else {
			filter_cat -= 6;
			if (filter_cat == 0) { // Freeware Apps
				if (p->requirements && strstr(p->requirements, "Game Data Files"))
					return true;
			} else {
				filter_cat--;
				if (filter_cat < 2) {
					if (p->state != filter_cat)
						return true;
				} else if (filter_cat == 2) { // Installed Apps
					if (p->state == APP_UNTRACKED)
						return true;
				} else {
					if (!p->trophies)
						return true;
				}
			}
		}
	}
	return false;
}

bool filterThemes(ThemeSelection *p) {
	switch (filter_idx) {
	case 1:
		if (p->state != APP_UPDATED)
			return true;
		break;
	case 2:
		if (p->state != APP_UNTRACKED)
			return true;
		break;
	default:
		break;
	}
	return false;
}

volatile bool kill_audio_thread = false;
static int musicThread(unsigned int args, void *arg) {
	// Starting background music
	SceUID f = sceIoOpen("ux0:/data/VitaDB/bg.ogg", SCE_O_RDONLY, 0777);
	int chn;
	Mix_Music *mus;
	if (f >= 0) {
		size_t size = sceIoLseek(f, 0, SCE_SEEK_END);
		sceIoLseek(f, 0, SCE_SEEK_SET);
		audio_buffer = vglMalloc(size);
		sceIoRead(f, audio_buffer, size);
		sceIoClose(f);
		SDL_RWops *rw = SDL_RWFromMem(audio_buffer, size);
		mus = Mix_LoadMUS_RW(rw, 0);
		Mix_PlayMusic(mus, -1);
	} else {
		return sceKernelExitDeleteThread(0);
	}
	while (!kill_audio_thread) {
		sceKernelDelayThread(1000);
	}
	Mix_HaltMusic();
	Mix_FreeMusic(mus);
	vglFree(audio_buffer);
	kill_audio_thread = false;
	return sceKernelExitDeleteThread(0);
}

float *bg_attributes = nullptr;
void DrawBackground() {
	if (!bg_attributes)
		bg_attributes = (float*)malloc(sizeof(float) * 22);

	if (has_animated_bg) {
		int anim_w, anim_h;
		GLuint anim_bg = video_get_frame(&anim_w, &anim_h);
		if (anim_bg == 0xDEADBEEF)
			return;
		glBindTexture(GL_TEXTURE_2D, anim_bg);
	} else {
		glBindTexture(GL_TEXTURE_2D, bg_image);
	}
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	
	bg_attributes[0] = 0.0f;
	bg_attributes[1] = 0.0f;
	bg_attributes[2] = 0.0f;
	bg_attributes[3] = 960.0f;
	bg_attributes[4] = 0.0f;
	bg_attributes[5] = 0.0f;
	bg_attributes[6] = 0.0f;
	bg_attributes[7] = 544.0f;
	bg_attributes[8] = 0.0f;
	bg_attributes[9] = 960.0f;
	bg_attributes[10] = 544.0f;
	bg_attributes[11] = 0.0f;
	vglVertexPointerMapped(3, bg_attributes);
	
	bg_attributes[12] = 0.0f;
	bg_attributes[13] = 0.0f;
	bg_attributes[14] = 1.0f;
	bg_attributes[15] = 0.0f;
	bg_attributes[16] = 0.0f;
	bg_attributes[17] = 1.0f;
	bg_attributes[18] = 1.0f;
	bg_attributes[19] = 1.0f;
	vglTexCoordPointerMapped(&bg_attributes[12]);
	
	uint16_t *bg_indices = (uint16_t*)&bg_attributes[20];
	bg_indices[0] = 0;
	bg_indices[1] = 1;
	bg_indices[2] = 2;
	bg_indices[3] = 3;
	vglIndexPointerMapped(bg_indices);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(0, 960, 544, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	vglDrawObjects(GL_TRIANGLE_STRIP, 4, GL_TRUE);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDisableClientState(GL_COLOR_ARRAY);
}

ImVec4 TextLabel = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
ImVec4 TextOutdated = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
ImVec4 TextUpdated = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
ImVec4 TextNotInstalled = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
ImVec4 Shuffle = ImVec4(0.0f, 0.0f, 1.0f, 0.4f);
ImVec4 ShuffleHovered = ImVec4(0.0f, 0.0f, 1.0f, 1.0f);
ImVec4 TextShadow = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
uint32_t TextShadowU32 = 0x00000000;

#define READ_FIRST_VAL(x) if (strcmp(#x, buffer) == 0) style.Colors[ImGuiCol_##x] = ImVec4(values[0], values[1], values[2], values[3]);
#define READ_NEXT_VAL(x) else if (strcmp(#x, buffer) == 0) style.Colors[ImGuiCol_##x] = ImVec4(values[0], values[1], values[2], values[3]);
#define READ_EXTRA_VAL(x) else if (strcmp(#x, buffer) == 0) x = ImVec4(values[0], values[1], values[2], values[3]);
#define WRITE_VAL(a) fprintf(f, "%s=%f,%f,%f,%f\n", #a, style.Colors[ImGuiCol_##a].x, style.Colors[ImGuiCol_##a].y, style.Colors[ImGuiCol_##a].z, style.Colors[ImGuiCol_##a].w);
#define WRITE_EXTRA_VAL(a) fprintf(f, "%s=%f,%f,%f,%f\n", #a, a.x, a.y, a.z, a.w);
void set_gui_theme() {
	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	ImVec4 col_area = ImVec4(0.047f, 0.169f, 0.059f, 0.44f);
	ImVec4 col_main = ImVec4(0.2f, 0.627f, 0.169f, 0.86f);
	FILE *f = fopen("ux0:data/VitaDB/theme.ini", "r");
	if (f) {
		float values[4];
		char buffer[64];
		while (EOF != fscanf(f, "%[^=]=%f,%f,%f,%f\n", buffer, &values[0], &values[1], &values[2], &values[3])) {
			READ_FIRST_VAL(FrameBg)
			READ_NEXT_VAL(FrameBgHovered)
			READ_NEXT_VAL(TitleBgActive)
			READ_NEXT_VAL(MenuBarBg)
			READ_NEXT_VAL(ScrollbarBg)
			READ_NEXT_VAL(ScrollbarGrab)
			READ_NEXT_VAL(Button)
			READ_NEXT_VAL(ButtonHovered)
			READ_NEXT_VAL(Header)
			READ_NEXT_VAL(HeaderHovered)
			READ_NEXT_VAL(NavHighlight)
			READ_NEXT_VAL(Text)
			READ_NEXT_VAL(Separator)
			READ_NEXT_VAL(PlotHistogram)
			READ_NEXT_VAL(WindowBg)
			READ_NEXT_VAL(Border)
			READ_EXTRA_VAL(TextLabel)
			READ_EXTRA_VAL(TextOutdated)
			READ_EXTRA_VAL(TextUpdated)
			READ_EXTRA_VAL(TextNotInstalled)
			READ_EXTRA_VAL(Shuffle)
			READ_EXTRA_VAL(ShuffleHovered)
			READ_EXTRA_VAL(TextShadow)
		}
		fclose(f);
	} else { // Save default theme
		f = fopen("ux0:data/VitaDB/theme.ini", "w");
		WRITE_VAL(FrameBg)
		WRITE_VAL(FrameBgHovered)
		WRITE_VAL(TitleBgActive)
		WRITE_VAL(MenuBarBg)
		WRITE_VAL(ScrollbarBg)
		WRITE_VAL(ScrollbarGrab)
		WRITE_VAL(Button)
		WRITE_VAL(ButtonHovered)
		WRITE_VAL(Header)
		WRITE_VAL(HeaderHovered)
		WRITE_VAL(NavHighlight)
		WRITE_VAL(Text)
		WRITE_VAL(Separator)
		WRITE_VAL(PlotHistogram)
		WRITE_VAL(WindowBg)
		WRITE_VAL(Border)
		WRITE_EXTRA_VAL(TextLabel)
		WRITE_EXTRA_VAL(TextOutdated)
		WRITE_EXTRA_VAL(TextUpdated)
		WRITE_EXTRA_VAL(TextNotInstalled)
		WRITE_EXTRA_VAL(Shuffle)
		WRITE_EXTRA_VAL(ShuffleHovered)
		WRITE_EXTRA_VAL(TextShadow)
		fclose(f);
	}
	
	TextShadowU32 = (uint32_t)(TextShadow.x * 255.0f) | (uint32_t)(TextShadow.y * 255.0f) << 8 | (uint32_t)(TextShadow.z * 255.0f) << 16 | (uint32_t)(TextShadow.w * 255.0f) << 24;
	if (TextShadowU32)
		ImGui::PushFontShadow(TextShadowU32);
	else
		ImGui::PopFontShadow();
}

void install_theme(ThemeSelection *g) {
	SceIoStat st;
	char fname[256];
	// Burning on screen the parsing text dialog
	for (int i = 0; i < 3; i++) {
		draw_text_dialog("Installing theme", true, false);
	}
	
	// Deleting old theme files
	sceIoRemove("ux0:data/VitaDB/bg.png");
	sceIoRemove("ux0:data/VitaDB/bg.mp4");
	sceIoRemove("ux0:data/VitaDB/bg.ogg");
	sceIoRemove("ux0:data/VitaDB/font.ttf");
	
	// Kill old audio playback
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	int res = sceKernelGetThreadInfo(audio_thd, &info);
	if (res >= 0) {
		kill_audio_thread = true;
		while (sceKernelGetThreadInfo(audio_thd, &info) >= 0) {
			sceKernelDelayThread(1000);
		}
	}

	//Start new background audio playback
	if (g->has_music[0] == '1') {
		sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.ogg", g->name);
		if (sceIoGetstat(fname, &st) >= 0) {
			copy_file(fname, "ux0:data/VitaDB/bg.ogg");
			audio_thd = sceKernelCreateThread("Audio Playback", &musicThread, 0x10000100, 0x100000, 0, 0, NULL);
			sceKernelStartThread(audio_thd, 0, NULL);
		}
	}
	
	// Kill old animated background
	if (has_animated_bg)
		video_close();
	
	// Load new background image
	switch (g->bg_type[0]) {
	case '1':
		sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.png", g->name);
		if (sceIoGetstat(fname, &st) >= 0)
			copy_file(fname, "ux0:data/VitaDB/bg.png");
		break;
	case '2':
		sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.mp4", g->name);
		if (sceIoGetstat(fname, &st) >= 0)
			copy_file(fname, "ux0:data/VitaDB/bg.mp4");
		break;
	default:
		break;
	}
	LoadBackground();
	
	// Set new color scheme
	sprintf(fname, "ux0:data/VitaDB/themes/%s/theme.ini", g->name);
	copy_file(fname, "ux0:data/VitaDB/theme.ini");
	set_gui_theme();
	
	// Set new font
	if (g->has_font[0] == '1') {
		sprintf(fname, "ux0:data/VitaDB/themes/%s/font.ttf", g->name);
		if (sceIoGetstat(fname, &st) >= 0)
			copy_file(fname, "ux0:data/VitaDB/font.ttf");
	}
	ImGui::GetIO().Fonts->Clear();
	ImGui_ImplVitaGL_InvalidateDeviceObjects();
	if (sceIoGetstat("ux0:/data/VitaDB/font.ttf", &st) >= 0)
		ImGui::GetIO().Fonts->AddFontFromFileTTF("ux0:/data/VitaDB/font.ttf", 16.0f);
	else
		ImGui::GetIO().Fonts->AddFontFromFileTTF("ux0:/app/VITADBDLD/Roboto.ttf", 16.0f);
}

void install_theme_from_shuffle(bool boot) {
	SceIoStat st;
	int themes_num = 0;
	
	FILE *f = fopen("ux0:data/VitaDB/shuffle.cfg", "r");
	while (EOF != fscanf(f, "%[^\n]\n", &generic_mem_buffer[20 * 1024 * 1024 + 256 * themes_num])) {
		themes_num++;
	}
	fclose(f);
	
	int theme_id = rand() % themes_num;
	char *name = (char *)&generic_mem_buffer[20 * 1024 * 1024 + 256 * theme_id];
	//printf("name is %s\n", name);
	
	if (!boot) {
		for (int i = 0; i < 3; i++) {
			draw_text_dialog("Installing random theme", true, false);
		}
	}
	
	// Deleting old theme files
	sceIoRemove("ux0:data/VitaDB/bg.png");
	sceIoRemove("ux0:data/VitaDB/bg.mp4");
	sceIoRemove("ux0:data/VitaDB/bg.ogg");
	sceIoRemove("ux0:data/VitaDB/font.ttf");

	// Kill old audio playback
	if (!boot) {
		SceKernelThreadInfo info;
		info.size = sizeof(SceKernelThreadInfo);
		int res = sceKernelGetThreadInfo(audio_thd, &info);
		if (res >= 0) {
			kill_audio_thread = true;
			while (sceKernelGetThreadInfo(audio_thd, &info) >= 0) {
				sceKernelDelayThread(1000);
			}
		}
	}

	//Start new background audio playback
	char fname[256];
	sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.ogg", name);
	if (sceIoGetstat(fname, &st) >= 0) {
		copy_file(fname, "ux0:data/VitaDB/bg.ogg");
		if (!boot) {
			audio_thd = sceKernelCreateThread("Audio Playback", &musicThread, 0x10000100, 0x100000, 0, 0, NULL);
			sceKernelStartThread(audio_thd, 0, NULL);
		}
	}

	// Kill old animated background
	if (has_animated_bg && !boot)
		video_close();

	// Load new background image
	sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.png", name);
	if (sceIoGetstat(fname, &st) >= 0)
		copy_file(fname, "ux0:data/VitaDB/bg.png");
	else {
		sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.mp4", name);
		if (sceIoGetstat(fname, &st) >= 0)
			copy_file(fname, "ux0:data/VitaDB/bg.mp4");
	}
	if (!boot)
		LoadBackground();

	// Set new color scheme
	sprintf(fname, "ux0:data/VitaDB/themes/%s/theme.ini", name);
	copy_file(fname, "ux0:data/VitaDB/theme.ini");
	if (!boot)
		set_gui_theme();

	// Set new font
	sprintf(fname, "ux0:data/VitaDB/themes/%s/font.ttf", name);
	if (sceIoGetstat(fname, &st) >= 0)
		copy_file(fname, "ux0:data/VitaDB/font.ttf");
	if (!boot) {
		ImGui::GetIO().Fonts->Clear();
		ImGui_ImplVitaGL_InvalidateDeviceObjects();
		if (sceIoGetstat("ux0:/data/VitaDB/font.ttf", &st) >= 0)
			ImGui::GetIO().Fonts->AddFontFromFileTTF("ux0:/data/VitaDB/font.ttf", 16.0f);
		else
			ImGui::GetIO().Fonts->AddFontFromFileTTF("ux0:/app/VITADBDLD/Roboto.ttf", 16.0f);
	}
}

enum {
	ENTRY_TYPE_FILE,
	ENTRY_TYPE_FOLDER
};

typedef struct {
	char name[512];
	uint8_t type;
	uint32_t size;
} entry;

#include <lz4.h>

int main(int argc, char *argv[]) {
	// Check if an on demand update has been requested
	sceAppMgrGetAppParam(boot_params);
	if (strlen(boot_params) > 0) {
		if (!strstr(boot_params, "psgm")) {
			update_detected = true;
		} else {
			boot_params[0] = 0;
		}
	}
	
	srand(time(NULL));
	SceIoStat st;
	
	sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir("ux0:data/VitaDB", 0777);
	sceIoMkdir("ux0:data/VitaDB/trophies", 0777);
	sceIoMkdir("ux0:pspemu", 0777);
	sceIoMkdir("ux0:pspemu/PSP", 0777);
	sceIoMkdir("ux0:pspemu/PSP/GAME", 0777);
	
	// Removing any failed app download leftover
	sceIoRemove(TEMP_DOWNLOAD_NAME);
	
	// Initializing sceNet
	generic_mem_buffer = (uint8_t *)memalign(64, MEM_BUFFER_SIZE);
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	int ret = sceNetShowNetstat();
	SceNetInitParam initparam;
	if (ret == SCE_NET_ERROR_ENOTINIT) {
		initparam.memory = malloc(141 * 1024);
		initparam.size = 141 * 1024;
		initparam.flags = 0;
		sceNetInit(&initparam);
	}
	
	// Initializing extractors
	init_read_buffer();
	
	// Initializing sceCommonDialog
	SceAppUtilInitParam init_params = {};
	SceAppUtilBootParam init_boot_params = {};
	sceAppUtilInit(&init_params, &init_boot_params);
	SceCommonDialogConfigParam cmnDlgCfgParam;
	sceCommonDialogConfigParamInit(&cmnDlgCfgParam);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cmnDlgCfgParam.language);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cmnDlgCfgParam.enterButtonAssign);
	if (cmnDlgCfgParam.enterButtonAssign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS)
		SCE_CTRL_CANCEL = SCE_CTRL_CIRCLE;
	sceCommonDialogSetConfigParam(&cmnDlgCfgParam);
	
	// Checking for network connection
	sceNetCtlInit();
	sceNetCtlInetGetState(&ret);
	if (ret != SCE_NETCTL_STATE_CONNECTED)
		early_fatal_error("Error: You need an Internet connection to run this application.");
	sceNetCtlTerm();
	
	// Checking for libshacccg.suprx existence
	bool use_ur0_config = false;
	uint8_t kubridge_state = KUBRIDGE_MISSING;
	char user_plugin_str[96];
	strcpy(user_plugin_str, "*SHARKF00D\nux0:data/vitadb.suprx\n*NPXS10031\nux0:data/vitadb.suprx\n");
	SceUID fp = -1;
	if (sceIoGetstat("ux0:tai/config.txt", &st) >= 0) {
		if (!SCE_S_ISDIR(st.st_mode)) {
			fp = sceIoOpen("ux0:tai/config.txt", SCE_O_RDONLY, 0777);
			//printf("using ux0 taiHEN config file\n");
		}
	}
	if (fp < 0) {
		fp = sceIoOpen("ur0:tai/config.txt", SCE_O_RDONLY, 0777);
		use_ur0_config = true;
	}
	int cfg_size = sceIoRead(fp, generic_mem_buffer, MEM_BUFFER_SIZE);
	sceIoClose(fp);
	if (!strncmp((const char *)generic_mem_buffer, user_plugin_str, strlen(user_plugin_str))) {
		if (!(sceIoGetstat("ur0:/data/libshacccg.suprx", &st) >= 0 || sceIoGetstat("ur0:/data/external/libshacccg.suprx", &st) >= 0)) { // Step 2: Extract libshacccg.suprx
extract_libshacccg:
			sceIoRemove("ux0:/data/Runtime1.00.pkg");
			sceIoRemove("ux0:/data/Runtime2.01.pkg");
			early_download_file("https://www.rinnegatamante.eu/vitadb/get_hb_url.php?id=567", "Downloading SharkF00D");
			sceIoMkdir(TEMP_INSTALL_DIR, 0777);
			early_extract_zip_file(TEMP_DOWNLOAD_NAME, TEMP_INSTALL_PATH);
			sceIoRemove(TEMP_DOWNLOAD_NAME);
			makeHeadBin(TEMP_INSTALL_DIR);
			init_warning("Installing SharkF00D");
			scePromoterUtilInit();
			scePromoterUtilityPromotePkg(TEMP_INSTALL_DIR, 0);
			int state = 0;
			do {
				vglSwapBuffers(GL_TRUE);
				int ret = scePromoterUtilityGetState(&state);
				if (ret < 0)
					break;
			} while (state);
			sceMsgDialogClose();
			int status = sceMsgDialogGetStatus();
			do {
				vglSwapBuffers(GL_TRUE);
				status = sceMsgDialogGetStatus();
			} while (status != SCE_COMMON_DIALOG_STATUS_FINISHED);
			sceMsgDialogTerm();
			scePromoterUtilTerm();
			sceAppMgrLaunchAppByName(0x60000, "SHARKF00D", "");
			sceKernelExitProcess(0);
		} else { // Step 3: Cleanup
			fp = sceIoOpen(use_ur0_config ? "ur0:tai/config.txt" : "ux0:tai/config.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
			sceIoWrite(fp, &generic_mem_buffer[strlen(user_plugin_str)], cfg_size - strlen(user_plugin_str));
			sceIoClose(fp);
			sceIoRemove("ux0:data/vitadb.skprx");
			sceIoRemove("ux0:data/vitadb.suprx");
#if 0 // On retail console, this causes the app to get minimized which we don't want to
			scePromoterUtilInit();
			scePromoterUtilityDeletePkg("SHARKF00D");
			int state = 0;
			do {
				int ret = scePromoterUtilityGetState(&state);
				if (ret < 0)
					break;
			} while (state);
			scePromoterUtilTerm();
#endif
		}
	} else if (!(sceIoGetstat("ur0:/data/libshacccg.suprx", &st) >= 0 || sceIoGetstat("ur0:/data/external/libshacccg.suprx", &st) >= 0)) { // Step 1: Download PSM Runtime and install it
		if (!(sceIoGetstat("ux0:/app/PCSI00011/runtime_version.txt", &st)) >= 0) { // PSM Runtime is not installed, downloading it
			early_warning("Runtime shader compiler (libshacccg.suprx) is not installed. VitaDB Downloader will proceed with its extraction.");
			void *tmp_buffer = malloc(cfg_size);
			sceClibMemcpy(tmp_buffer, generic_mem_buffer, cfg_size);
			early_download_file("https://archive.org/download/psm-runtime/IP9100-PCSI00011_00-PSMRUNTIME000000.pkg", "Downloading PSM Runtime v.1.00");
			sceIoRename(TEMP_DOWNLOAD_NAME, "ux0:/data/Runtime1.00.pkg");
			early_download_file("https://archive.org/download/psm-runtime/IP9100-PCSI00011_00-PSMRUNTIME000000-A0201-V0100-e4708b1c1c71116c29632c23df590f68edbfc341-PE.pkg", "Downloading PSM Runtime v.2.01");
			sceIoRename(TEMP_DOWNLOAD_NAME, "ux0:/data/Runtime2.01.pkg");
			fp = sceIoOpen(use_ur0_config ? "ur0:tai/config.txt" : "ux0:tai/config.txt", SCE_O_CREAT | SCE_O_TRUNC | SCE_O_WRONLY, 0777);
			copy_file("app0:vitadb.skprx", "ux0:data/vitadb.skprx");
			copy_file("app0:vitadb.suprx", "ux0:data/vitadb.suprx");
			sceIoWrite(fp, user_plugin_str, strlen(user_plugin_str));
			sceIoWrite(fp, tmp_buffer, cfg_size);
			sceIoClose(fp);
			free(tmp_buffer);
			taiLoadStartKernelModule("ux0:data/vitadb.skprx", 0, NULL, 0);
			sceAppMgrLaunchAppByName(0x60000, "NPXS10031", "[BATCH]host0:/package/Runtime1.00.pkg\nhost0:/package/Runtime2.01.pkg");
			sceKernelExitProcess(0);
		} else { // PSM Runtime already installed, we skip directly to SHARKF00D installation
			goto extract_libshacccg;
		}
	}
	
	// Remove any leftover from libshacccg.suprx extraction
	sceIoRemove("ux0:data/vitadb.skprx");
	sceIoRemove("ux0:data/vitadb.suprx");
	
	// Check for kubridge existence
	if (strstr((const char *)generic_mem_buffer, "kubridge.skprx"))
		kubridge_state = sceIoGetstat("ur0:/tai/kubridge.skprx", &st) >= 0 ? KUBRIDGE_UR0 : KUBRIDGE_UX0;
	
	// Initializing SDL and SDL mixer
	SDL_Init(SDL_INIT_AUDIO);
	Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 512);
	Mix_AllocateChannels(4);
	
	// Removing any failed app installation leftover
	if (sceIoGetstat(TEMP_INSTALL_DIR, &st) >= 0)
		recursive_rmdir(TEMP_INSTALL_DIR);
	if (sceIoGetstat(TEMP_DATA_DIR, &st) >= 0)
		recursive_rmdir(TEMP_DATA_DIR);
	
	// Initializing sceAppUtil
	SceAppUtilInitParam appUtilParam;
	SceAppUtilBootParam appUtilBootParam;
	memset(&appUtilParam, 0, sizeof(SceAppUtilInitParam));
	memset(&appUtilBootParam, 0, sizeof(SceAppUtilBootParam));
	sceAppUtilInit(&appUtilParam, &appUtilBootParam);
	
	// Initializing vitaGL
	AppSelection *hovered = nullptr;
	vglInitExtended(0, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_NONE);
	prepare_simple_drawer();
	prepare_bubble_drawer();

	// Apply theme shuffling
	if (sceIoGetstat("ux0:/data/VitaDB/shuffle.cfg", &st) >= 0)
		install_theme_from_shuffle(true);
	LoadBackground();

	// Load trophy icon
	int w, h;
	uint8_t *trp_data = stbi_load("app0:trophy.png", &w, &h, NULL, 4);
	glGenTextures(1, &trp_icon);
	glBindTexture(GL_TEXTURE_2D, trp_icon);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, trp_data);
	free(trp_data);
	glGenTextures(1, &empty_icon);

	// Initializing dear ImGui
	ImGui::CreateContext();
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	int res = 0;
	ImGui_ImplVitaGL_Init_Extended();
	if (sceIoGetstat("ux0:/data/VitaDB/font.ttf", &st) >= 0)
		ImGui::GetIO().Fonts->AddFontFromFileTTF("ux0:/data/VitaDB/font.ttf", 16.0f);
	else
		ImGui::GetIO().Fonts->AddFontFromFileTTF("app0:/Roboto.ttf", 16.0f);
	set_gui_theme();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::GetIO().MouseDrawCursor = false;
	ImGui_ImplVitaGL_TouchUsage(false);
	ImGui_ImplVitaGL_GamepadUsage(true);
	ImGui_ImplVitaGL_MouseStickUsage(false);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
	
	// Start background audio playback
	audio_thd = sceKernelCreateThread("Audio Playback", &musicThread, 0x10000100, 0x100000, 0, 0, NULL);
	sceKernelStartThread(audio_thd, 0, NULL);
	
	// Daemon popup
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(0, &pad, 1);
	if ((pad.buttons & SCE_CTRL_LTRIGGER) || sceIoGetstat("ux0:data/VitaDB/daemon.cfg", &st) < 0) {
		SceUID fp = sceIoOpen(use_ur0_config ? "ur0:tai/config.txt" : "ux0:tai/config.txt", SCE_O_RDONLY, 0777);
		size_t len = sceIoRead(fp, &generic_mem_buffer[20 * 1024 * 1024], MEM_BUFFER_SIZE);
		sceIoClose(fp);
		if (!strstr((const char *)&generic_mem_buffer[20 * 1024 * 1024], "ux0:data/VitaDB/vdb_daemon.suprx")) {
			init_interactive_msg_dialog("VitaDB Downloader features a functionality that allows the homebrew to check automatically, every hour and at every console bootup, if an update for an installed homebrew is available. A plugin is required to enable this feature, do you want to install it?");
			while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
				draw_simple_texture(previous_frame);
				vglSwapBuffers(GL_TRUE);
			}
			SceMsgDialogResult msg_res;
			memset(&msg_res, 0, sizeof(SceMsgDialogResult));
			sceMsgDialogGetResult(&msg_res);
			sceMsgDialogTerm();
			if (msg_res.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
				copy_file("app0:vdb_daemon.suprx", "ux0:data/VitaDB/vdb_daemon.suprx");
				fp = sceIoOpen(use_ur0_config ? "ur0:tai/config.txt" : "ux0:tai/config.txt", SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0777);
				strcpy(user_plugin_str, "*main\nux0:data/VitaDB/vdb_daemon.suprx\n");
				sceIoWrite(fp, user_plugin_str, strlen(user_plugin_str));
				sceIoWrite(fp, &generic_mem_buffer[20 * 1024 * 1024], len);
				sceIoClose(fp);
			}
		}
		fp = sceIoOpen("ux0:data/VitaDB/daemon.cfg", SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0777);
		sceIoClose(fp);
	}
	
	// Checking for VitaDB Daemon updates
	if (sceIoGetstat("ux0:data/VitaDB/vdb_daemon.suprx", &st) >= 0) {
		SceUID f = sceIoOpen("ux0:data/VitaDB/vdb_daemon.suprx", SCE_O_RDONLY, 0777);
		char cur_hash[40], new_hash[40];
		calculate_md5(f, cur_hash);
		f = sceIoOpen("app0:vdb_daemon.suprx", SCE_O_RDONLY, 0777);
		calculate_md5(f, new_hash);
		if (strncmp(cur_hash, new_hash, 32)) {
			copy_file("app0:vdb_daemon.suprx", "ux0:data/VitaDB/vdb_daemon.suprx");
		}
	}
	
	// Downloading icons
	if ((sceIoGetstat("ux0:/data/VitaDB/icons.db", &st) < 0) || (sceIoGetstat("ux0:data/VitaDB/icons", &st) < 0)) {
		download_file("https://www.rinnegatamante.eu/vitadb/icons_zip.php", "Downloading apps icons");
		sceIoMkdir("ux0:data/VitaDB/icons", 0777);
		extract_zip_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/icons/", true);
		sceIoRemove(TEMP_DOWNLOAD_NAME);
	} else if (sceIoGetstat("ux0:/data/VitaDB/icons/0b", &st) < 0) {
		// Checking if old icons system is being used and upgrade it
		for (int i = 0; i < 3; i++) {
			draw_text_dialog("Upgrading icons system, please wait...", true, false);
		}
		recursive_rmdir("ux0:data/VitaDB/icons");
		download_file("https://www.rinnegatamante.eu/vitadb/icons_zip.php", "Downloading apps icons");
		sceIoMkdir("ux0:data/VitaDB/icons", 0777);
		extract_zip_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/icons/", true);
		sceIoRemove(TEMP_DOWNLOAD_NAME);
	}
	
	
	// Downloading apps list
	sceAppMgrUmount("app0:");
	if (strlen(boot_params) == 0) {
		if (!(pad.buttons & SCE_CTRL_RTRIGGER) || sceIoGetstat("ux0:data/VitaDB/apps.json", &st) < 0) {
			SceUID thd = sceKernelCreateThread("Apps List Downloader", &appListThread, 0x10000100, 0x100000, 0, 0, NULL);
			sceKernelStartThread(thd, 0, NULL);
			do {
				draw_downloader_dialog(downloader_pass, downloaded_bytes, total_bytes, "Downloading apps list", 1, true);
				res = sceKernelGetThreadInfo(thd, &info);
			} while (info.status <= SCE_THREAD_DORMANT && res >= 0);
		}
		populate_apps_database("ux0:data/VitaDB/apps.json", false);
	} else {
		populate_apps_database("ux0:data/vitadb.json", false);
	}
	
	// Initializing remaining stuffs
	populate_pspemu_path();
	char *changelog = nullptr;
	char right_str[64];
	bool show_changelog = false;
	bool show_requirements = false;
	bool calculate_right_len = true;
	bool go_to_top = false;
	bool fast_increment = false;
	bool fast_decrement = false;
	bool extra_menu_invoked = false;
	bool has_touched = false;
	bool is_app_hovered;
	float right_len = 0.0f;
	float text_diff_len = 0.0f;
	uint32_t oldpad;
	int filtered_entries;
	AppSelection *decrement_stack[4096];
	AppSelection *decremented_app = nullptr;
	ThemeSelection *to_install = nullptr;
	int decrement_stack_idx = 0;
	
	while (!update_detected) {
		if (old_sort_idx != sort_idx) {
			old_sort_idx = sort_idx;
			if (mode_idx == MODE_THEMES)
				sort_themes_list(&themes, sort_idx);
			else
				sort_apps_list(mode_idx == MODE_VITA_HBS ? &apps : &psp_apps, sort_idx);
		}
		
		if (bg_image || has_animated_bg) {
			if (show_trailer == 2 && has_animated_bg) {
				glClear(GL_COLOR_BUFFER_BIT);
			} else {
				DrawBackground();
			}
		}
		
		ImGui_ImplVitaGL_NewFrame();
		
		if (ImGui::BeginMainMenuBar()) {
			char title[256];
			if (mode_idx == MODE_THEMES)
				sprintf(title, "VitaDB Downloader v.%s - Currently listing %d themes with '%s' filter", VERSION, filtered_entries, filter_themes_modes[filter_idx]);
			else
				sprintf(title, "VitaDB Downloader v.%s - Currently listing %d results with '%s' filter", VERSION, filtered_entries, filter_modes[filter_idx]);
			ImGui::Text(title);
			if (calculate_right_len) {
				calculate_right_len = false;
				sprintf(right_str, "%s", modes[mode_idx]);
				ImVec2 right_sizes = ImGui::CalcTextSize(right_str);
				right_len = right_sizes.x;
			}
			ImGui::SetCursorPosX(950 - right_len);
			ImGui::TextColored(TextLabel, right_str); 
			ImGui::EndMainMenuBar();
		}
		
		if (bg_image || has_animated_bg)
			ImGui::SetNextWindowBgAlpha(0.3f);
		ImGui::SetNextWindowPos(ImVec2(0, 21), ImGuiSetCond_Always);
		ImGui::SetNextWindowSize(ImVec2(553, 523), ImGuiSetCond_Always);
		ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
		
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Search: ");
		ImGui::SameLine();
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
		if (ImGui::Button(app_name_filter, ImVec2(-1.0f, 0.0f))) {
			init_interactive_ime_dialog("Insert search term", app_name_filter);
		}
		if (ImGui::IsItemHovered()) {
			hovered = nullptr;
			old_hovered = nullptr;
		}
		if (go_to_top) {
			ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
			ImGui::SetScrollHere();
			go_to_top = false;
			hovered = nullptr;
			old_hovered = nullptr;
		}
		ImGui::PopStyleVar();
		ImGui::AlignTextToFramePadding();
		if (text_diff_len == 0.0f)
			text_diff_len = ImGui::CalcTextSize("Search: ").x - ImGui::CalcTextSize("Filter: ").x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + text_diff_len);
		ImGui::Text("Filter: ");
		ImGui::SameLine();
		ImGui::PushItemWidth(190.0f);
		if (mode_idx == MODE_THEMES) {
			if (ImGui::BeginCombo("##combo", filter_themes_modes[filter_idx])) {
				for (int n = 0; n < sizeof(filter_themes_modes) / sizeof(*filter_themes_modes); n++) {
					bool is_selected = filter_idx == n;
					if (ImGui::Selectable(filter_themes_modes[n], is_selected))
						filter_idx = n;
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		} else {
			if (ImGui::BeginCombo("##combo", filter_modes[filter_idx])) {
				for (int n = 0; n < sizeof(filter_modes) / sizeof(*filter_modes); n++) {
					bool is_selected = filter_idx == n;
					if (ImGui::Selectable(filter_modes[n], is_selected))
						filter_idx = n;
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}
		if (ImGui::IsItemHovered()) {
			hovered = nullptr;
			old_hovered = nullptr;
		}
		ImGui::PopItemWidth();
		ImGui::AlignTextToFramePadding();
		ImGui::SameLine();
		ImGui::Spacing();
		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
		ImGui::Text("Sort Mode: ");
		ImGui::SameLine();
		ImGui::PushItemWidth(-1.0f);
		if (mode_idx == MODE_THEMES) {
			if (ImGui::BeginCombo("##combo2", sort_modes_themes_str[sort_idx])) {
				for (int n = 0; n < sizeof(sort_modes_themes_str) / sizeof(*sort_modes_themes_str); n++) {
					bool is_selected = sort_idx == n;
					if (ImGui::Selectable(sort_modes_themes_str[n], is_selected))
						sort_idx = n;
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		} else {
			if (ImGui::BeginCombo("##combo2", sort_modes_str[sort_idx])) {
				for (int n = 0; n < sizeof(sort_modes_str) / sizeof(*sort_modes_str); n++) {
					bool is_selected = sort_idx == n;
					if (ImGui::Selectable(sort_modes_str[n], is_selected))
						sort_idx = n;
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}
		if (ImGui::IsItemHovered()) {
			hovered = nullptr;
			old_hovered = nullptr;
		}
		ImGui::PopItemWidth();
		ImGui::Separator();
		
		if (mode_idx == MODE_THEMES) {
			ThemeSelection *g = themes;
			filtered_entries = 0;
			int increment_idx = 0;
			is_app_hovered = false;
			ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
			while (g) {
				if (filterThemes(g)) {
					g = g->next;
					continue;
				}
				if ((strlen(app_name_filter) == 0) || (strlen(app_name_filter) > 0 && (strcasestr(g->name, app_name_filter) || strcasestr(g->author, app_name_filter)))) {
					float y = ImGui::GetCursorPosY() + 3.0f;
					bool is_shuffle = false;
					if (g->shuffle) {
						is_shuffle = true;
						ImGui::PushStyleColor(ImGuiCol_Button, Shuffle);
						ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ShuffleHovered);
					}
					if (ImGui::Button(g->name, ImVec2(-1.0f, 0.0f))) {
						if (g->state == APP_UNTRACKED)
							to_download = (AppSelection *)g;
						else if (shuffle_themes)
							g->shuffle = !g->shuffle;
						else
							to_install = g;
					}
					if (is_shuffle) {
						ImGui::PopStyleColor(2);
					}
					if (ImGui::IsItemHovered()) {
						is_app_hovered = true;
						hovered = (AppSelection *)g;
						if (fast_increment)
							increment_idx = 1;
						else if (fast_decrement) {
							if (decrement_stack_idx == 0)
								fast_decrement = false;
							else
								decremented_app = decrement_stack[decrement_stack_idx >= 20 ? (decrement_stack_idx - 20) : 0];
						}
					} else if (increment_idx) {
						increment_idx++;
						ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
						ImGui::SetScrollHere();
						if (increment_idx == 21 || g->next == nullptr)
							increment_idx = 0;
					} else if (fast_decrement) {
						if (!decremented_app)
							decrement_stack[decrement_stack_idx++] = (AppSelection *)g;
						else if (decremented_app == (AppSelection *)g) {
							ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
							ImGui::SetScrollHere();
							fast_decrement = false;
						}	
					}
					ImVec2 tag_len;
					switch (g->state) {
					case APP_UNTRACKED:
						tag_len = ImGui::CalcTextSize("Not Downloaded");
						ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
						ImGui::TextColored(TextNotInstalled, "Not Downloaded");
						break;
					case APP_UPDATED:
						tag_len = ImGui::CalcTextSize("Downloaded");
						ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
						ImGui::TextColored(TextUpdated, "Downloaded");
						break;
					default:
						tag_len = ImGui::CalcTextSize("Unknown");
						ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
						ImGui::Text("Unknown");
						break;
					}
					filtered_entries++;
				}
				g = g->next;
			}
		} else {
			AppSelection *g = mode_idx == MODE_VITA_HBS ? apps : psp_apps;
			filtered_entries = 0;
			int increment_idx = 0;
			int btn_idx = 0;
			is_app_hovered = false;
			ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
			while (g) {
				if (filterApps(g)) {
					g = g->next;
					continue;
				}
				if ((strlen(app_name_filter) == 0) || (strlen(app_name_filter) > 0 && (strcasestr(g->name, app_name_filter) || strcasestr(g->author, app_name_filter)))) {
					float y = ImGui::GetCursorPosY() + 3.0f;
					if (g->trophies) {
						char lbl[128];
						sprintf(lbl, "##%d", btn_idx++);
						if (ImGui::Button(lbl, ImVec2(-1.0f, 0.0f))) {
							to_download = g;
						}
					} else {
						if (ImGui::Button(g->name, ImVec2(-1.0f, 0.0f))) {
							to_download = g;
						}
					}
					if (ImGui::IsItemHovered()) {
						is_app_hovered = true;
						hovered = g;
						if (fast_increment)
							increment_idx = 1;
						else if (fast_decrement) {
							if (decrement_stack_idx == 0)
								fast_decrement = false;
							else
								decremented_app = decrement_stack[decrement_stack_idx >= 20 ? (decrement_stack_idx - 20) : 0];
						}
					} else if (increment_idx) {
						increment_idx++;
						ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
						ImGui::SetScrollHere();
						if (increment_idx == 21 || g->next == nullptr)
							increment_idx = 0;
					} else if (fast_decrement) {
						if (!decremented_app)
							decrement_stack[decrement_stack_idx++] = g;
						else if (decremented_app == g) {
							ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
							ImGui::SetScrollHere();
							fast_decrement = false;
						}	
					}
					ImVec2 tag_len;
					switch (g->state) {
					case APP_UNTRACKED:
						tag_len = ImGui::CalcTextSize("Not Installed");
						ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
						ImGui::TextColored(TextNotInstalled, "Not Installed");
						break;
					case APP_OUTDATED:
						tag_len = ImGui::CalcTextSize("Outdated");
						ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
						ImGui::TextColored(TextOutdated, "Outdated");
						break;
					case APP_UPDATED:
						tag_len = ImGui::CalcTextSize("Updated");
						ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
						ImGui::TextColored(TextUpdated, "Updated");
						break;
					default:
						tag_len = ImGui::CalcTextSize("Unknown");
						ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
						ImGui::Text("Unknown");
						break;
					}
					if (g->trophies) {
						ImGui::SetCursorPosY(y - 2.0f);
						ImGui::Image((void*)trp_icon, ImVec2(20, 20));
						ImGui::SetCursorPos(ImVec2(28.0f, y));
						ImGui::Text(g->name);
					}
					filtered_entries++;
				}
				g = g->next;
			}
		}
		ImGui::PopStyleVar();
		if (decrement_stack_idx == filtered_entries || !is_app_hovered)
			fast_decrement = false;
		fast_increment = false;
		ImGui::End();
		
		if (bg_image || has_animated_bg)
			ImGui::SetNextWindowBgAlpha(0.3f);
		ImGui::SetNextWindowPos(ImVec2(553, 21), ImGuiSetCond_Always);
		ImGui::SetNextWindowSize(ImVec2(407, 523), ImGuiSetCond_Always);
		ImGui::Begin("Info Window", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
		if (hovered) {
			if (mode_idx == MODE_THEMES) {
				LoadPreview(hovered);
				ImGui::SetCursorPos(ImVec2(preview_x + PREVIEW_PADDING_THEME, preview_y + PREVIEW_PADDING));
				ImGui::Image((void*)preview_icon, ImVec2(preview_width, preview_height));
				ThemeSelection *node = (ThemeSelection *)hovered;
				ImGui::TextColored(TextLabel, "Author:");
				ImGui::TextWrapped(node->author);
				ImGui::TextColored(TextLabel, "Description:");
				ImGui::TextWrapped(node->desc);
				ImGui::TextColored(TextLabel, "Credits:");
				ImGui::TextWrapped(node->credits);
				ImGui::Separator();
				ImGui::TextColored(TextLabel, "Background Type: ");
				ImGui::SameLine();
				switch (node->bg_type[0]) {
				case '0':
					ImGui::Text("Static Color");
					break;
				case '1':
					ImGui::Text("Static Image");
					break;
				case '2':
					ImGui::Text("Animated Image");
					break;
				default:
					ImGui::Text("Unknown");
					break;
				}
				ImGui::TextColored(TextLabel, "Background Music: ");
				ImGui::SameLine();
				ImGui::Text(node->has_music[0] == '1' ? "Yes" : "No");
				ImGui::TextColored(TextLabel, "Custom Font: ");
				ImGui::SameLine();
				ImGui::Text(node->has_font[0] == '1' ? "Yes" : "No");
				ImGui::SetCursorPosY(438);
				ImGui::TextColored(TextLabel, "Press Start to view screenshots");
			} else {
				LoadPreview(hovered);
				ImGui::SetCursorPos(ImVec2(preview_x + PREVIEW_PADDING, preview_y + PREVIEW_PADDING));
				if (mode_idx == MODE_VITA_HBS) {
					ImGui::Image((void*)draw_bubble_icon(preview_icon), ImVec2(preview_width, preview_height));
					ImGui::SetCursorPosY(100);
					ImGui::SetCursorPosX(140);
				} else {
					ImGui::Image((void*)preview_icon, ImVec2(preview_width, preview_height));
				}
				ImGui::TextColored(TextLabel, "Size:");
				if (mode_idx == MODE_VITA_HBS) {
					ImGui::SetCursorPosY(116);
					ImGui::SetCursorPosX(140);
				}
				char size_str[64];
				char *dummy;
				uint64_t sz;
				if (strlen(hovered->data_link) > 5) {
					sz = strtoull(hovered->size, &dummy, 10);
					uint64_t sz2 = strtoull(hovered->data_size, &dummy, 10);
					sprintf(size_str, "%s: %.2f %s, Data: %.2f %s", mode_idx == MODE_VITA_HBS ? "VPK" : "App", format_size(sz), format_size_str(sz), format_size(sz2), format_size_str(sz2));
				} else {
					sz = strtoull(hovered->size, &dummy, 10);
					sprintf(size_str, "%s: %.2f %s", mode_idx == MODE_VITA_HBS ? "VPK" : "App", format_size(sz), format_size_str(sz));
				}
				ImGui::Text(size_str);
				ImGui::TextColored(TextLabel, "Description:");
				ImGui::TextWrapped(hovered->desc);
				ImGui::SetCursorPosY(6);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				ImGui::TextColored(TextLabel, "Last Update:");
				ImGui::SetCursorPosY(22);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				ImGui::Text(hovered->date);
				ImGui::SetCursorPosY(6);
				ImGui::SetCursorPosX(320);
				ImGui::TextColored(TextLabel, "Downloads:");
				ImGui::SetCursorPosY(22);
				ImGui::SetCursorPosX(320);
				ImGui::Text(hovered->downloads);
				if (mode_idx == MODE_VITA_HBS) {
					ImGui::SetCursorPosY(38);
					ImGui::SetCursorPosX(320);
					ImGui::TextColored(TextLabel, "TitleID:");
					ImGui::SetCursorPosY(56);
					ImGui::SetCursorPosX(320);
					if (hovered->next_clash || hovered->prev_clash)
						ImGui::TextColored(TextOutdated, hovered->titleid);
					else
						ImGui::Text(hovered->titleid);
				}
				ImGui::SetCursorPosY(38);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				ImGui::TextColored(TextLabel, "Category:");
				ImGui::SetCursorPosY(54);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				switch (hovered->type[0]) {
				case '1':
					ImGui::Text("Original Game");
					break;
				case '2':
					ImGui::Text("Game Port");
					break;
				case '4':
					ImGui::Text("Utility");
					break;		
				case '5':
					ImGui::Text("Emulator");
					break;
				default:
					ImGui::Text("Unknown");
					break;
				}
				ImGui::SetCursorPosY(70);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				ImGui::TextColored(TextLabel, "Author:");
				ImGui::SetCursorPosY(86);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				ImGui::Text(hovered->author);
				ImGui::SetCursorPosY(454);
				if (strlen(hovered->trailer) > 5) {
					ImGui::TextColored(TextLabel, "Press Start to view the trailer");
				} else if (strlen(hovered->screenshots) > 5) {
					ImGui::TextColored(TextLabel, "Press Start to view screenshots");
				}
				ImGui::SetCursorPosY(470);
				ImGui::TextColored(TextLabel, "Press Select for more options");
			}
		}
		if (mode_idx == MODE_THEMES) {
			ImGui::SetCursorPosY(454);
			ImGui::TextColored(TextLabel, "Press Select to change themes mode");
			ImGui::SetCursorPosY(470);
			ImGui::Text("Current theme mode: %s", shuffle_themes ? "Shuffle" : "Single");
			ImGui::SetCursorPosY(486);
			ImGui::Text("Current sorting mode: %s", sort_modes_themes_str[sort_idx]);
		} else {
			ImGui::SetCursorPosY(486);
			ImGui::Text("Current sorting mode: %s", sort_modes_str[sort_idx]);
		}
		ImGui::SetCursorPosY(502);
		uint64_t total_space = get_total_storage();
		uint64_t free_space = get_free_storage();
		ImGui::Text("Free storage: %.2f %s / %.2f %s", format_size(free_space), format_size_str(free_space), format_size(total_space), format_size_str(total_space));
		ImGui::End();
		
		if (extra_menu_invoked) {
			int num_items;
			switch (hovered->state) {
			case APP_OUTDATED: // Launch, Update, Screenshots (if any), Changelog, Uninstall, Tag Update
				num_items = mode_idx == MODE_VITA_HBS ? 5 : 4; // FIXME: Add PSP hbs launch via Adrenaline
				break;
			case APP_UPDATED: // Launch, Screenshots (if any), Changelog, Uninstall
				num_items = mode_idx == MODE_VITA_HBS ? 3 : 2; // FIXME: Add PSP hbs launch via Adrenaline
				break;
			case APP_UNTRACKED: // Install, Screenshots (if any), Changelog
				num_items = 2;
				break;
			default:
				printf("Fatal error\n");
				break;
			}
			if (hovered->requirements)
				num_items++;
			if (strlen(hovered->screenshots) > 5)
				num_items++;
			if (strlen(hovered->trailer) > 5)
				num_items++;
			if (strlen(hovered->source_page) > 5)
				num_items++;
			if (strlen(hovered->release_page) > 5)
				num_items++;
			int h = 29 + 25 * num_items;
			int y = 272 - h / 2;
			ImGui::SetNextWindowPos(ImVec2(280, y), ImGuiSetCond_Always);
			ImGui::SetNextWindowSize(ImVec2(400, h), ImGuiSetCond_Always);
			char titlebar[256];
			sprintf(titlebar, "%s - Manage", hovered->name);
			ImGui::Begin(titlebar, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
			if (mode_idx == MODE_VITA_HBS && hovered->state != APP_UNTRACKED) {
				if (ImGui::Button("Launch", ImVec2(-1.0f, 0.0f))) {
					sceAppMgrLaunchAppByName(0x60000, hovered->titleid, "");
					sceKernelExitProcess(0);
				}
			}
			if (hovered->state == APP_UNTRACKED) {
				if (ImGui::Button("Install", ImVec2(-1.0f, 0.0f))) {
					to_download = hovered;
					extra_menu_invoked = false;
				}
			} else {
				if (hovered->state == APP_OUTDATED) {
					if (ImGui::Button("Update", ImVec2(-1.0f, 0.0f))) {
						to_download = hovered;
						extra_menu_invoked = false;
					}
				}
				if (ImGui::Button("Uninstall", ImVec2(-1.0f, 0.0f))) {
					to_uninstall = hovered;
					extra_menu_invoked = false;
				}
			}
			if (hovered->state == APP_OUTDATED) {
				if (ImGui::Button("Tag as Updated", ImVec2(-1.0f, 0.0f))) {
					char fname[256];
					if (mode_idx == MODE_VITA_HBS) {
						sprintf(fname, "ux0:app/%s/hash.vdb", hovered->titleid);
					} else {
						sprintf(fname, "%spspemu/PSP/GAME/%s/hash.vdb", pspemu_dev, hovered->id);
					}
					SceUID f = sceIoOpen(fname, SCE_O_CREAT | SCE_O_TRUNC | SCE_O_WRONLY, 0777);
					sceIoWrite(f, hovered->hash, 32);
					sceIoClose(f);
					hovered->state = APP_UPDATED;
				}
			}
			if (hovered->trophies) {
				if (ImGui::Button("View Available Trophies", ImVec2(-1.0f, 0.0f))) {
					show_trophies = 1;
				}
			}
			if (hovered->requirements) {
				if (ImGui::Button("View Homebrew Requirements", ImVec2(-1.0f, 0.0f))) {
					show_requirements = true;
				}
			}
			if (strlen(hovered->screenshots) > 5) {
				if (ImGui::Button("View Screenshots", ImVec2(-1.0f, 0.0f))) {
					show_screenshots = 1;
				}
			}
			if (strlen(hovered->trailer) > 5) {
				if (ImGui::Button("View Trailer", ImVec2(-1.0f, 0.0f))) {
					show_trailer = 1;
				}
			}
			if (strlen(hovered->source_page) > 5) {
				if (ImGui::Button("View Sourcecode Page", ImVec2(-1.0f, 0.0f))) {
					SceAppUtilWebBrowserParam webparam;
					char url[512];
					sprintf(url, "http://www.rinnegatamante.eu/vitadb/get_page.php?id=%s&type=src", hovered->id);
					webparam.str = url;
					webparam.strlen = strlen(url);
					webparam.launchMode = 1;
					sceAppUtilLaunchWebBrowser(&webparam);
				}
			}
			if (strlen(hovered->release_page) > 5) {
				if (ImGui::Button("View Release Page", ImVec2(-1.0f, 0.0f))) {
					SceAppUtilWebBrowserParam webparam;
					char url[512];
					sprintf(url, "http://www.rinnegatamante.eu/vitadb/get_page.php?id=%s&type=rel", hovered->id);
					webparam.str = url;
					webparam.strlen = strlen(url);
					webparam.launchMode = 1;
					sceAppUtilLaunchWebBrowser(&webparam);
				}
			}
			if (ImGui::Button("View Changelog", ImVec2(-1.0f, 0.0f))) {
				show_changelog = true;
				changelog = get_changelog("ux0:data/VitaDB/apps.json", hovered->id);
			}
			ImGui::End();
		}
		
		if (show_screenshots == 2) {
			LoadScreenshot();
			ImGui::SetNextWindowPos(ImVec2(80, 55), ImGuiSetCond_Always);
			ImGui::SetNextWindowSize(ImVec2(800, 472), ImGuiSetCond_Always);
			if (SCE_CTRL_CANCEL == SCE_CTRL_CIRCLE) {
				ImGui::Begin("Screenshots Viewer (Left/Right to change current screenshot, Start or Circle to close)", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			} else {
				ImGui::Begin("Screenshots Viewer (Left/Right to change current screenshot, Start or Cross to close)", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			}
			ImGui::Image((void*)preview_shot, ImVec2(800 - 19, 453 - 19));
			ImGui::End();
		}
		
		if (show_trailer == 2) {
			ImGui::SetNextWindowPos(ImVec2(80, 55), ImGuiSetCond_Always);
			ImGui::SetNextWindowSize(ImVec2(800, 472), ImGuiSetCond_Always);
			if (SCE_CTRL_CANCEL == SCE_CTRL_CIRCLE) {
				ImGui::Begin("Trailer Viewer (Start or Circle to close)", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			} else {
				ImGui::Begin("Trailer Viewer (Start or Cross to close)", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);				
			}
			int vid_w, vid_h;
			GLuint vid_frame = video_get_frame(&vid_w, &vid_h);
			if (vid_frame != 0xDEADBEEF)
				ImGui::Image((void*)vid_frame, ImVec2(800 - 19, 453 - 19));
			ImGui::End();
		}
		
		if (show_changelog) {
			char titlebar[256];
			sprintf(titlebar, "%s Changelog (%s to close)", hovered->name, SCE_CTRL_CANCEL == SCE_CTRL_CIRCLE ? "Circle" : "Cross");
			ImGui::SetNextWindowPos(ImVec2(80, 55), ImGuiSetCond_Always);
			ImGui::SetNextWindowSize(ImVec2(800, 472), ImGuiSetCond_Always);
			ImGui::Begin(titlebar, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			ImGui::TextWrapped(changelog ? changelog : "- First Release.");
			ImGui::End();
		}
		
		if (show_requirements) {
			char titlebar[256];
			sprintf(titlebar, "%s Requirements (%s to close)", hovered->name, SCE_CTRL_CANCEL == SCE_CTRL_CIRCLE ? "Circle" : "Cross");
			ImGui::SetNextWindowPos(ImVec2(80, 55), ImGuiSetCond_Always);
			ImGui::SetNextWindowSize(ImVec2(800, 472), ImGuiSetCond_Always);
			ImGui::Begin(titlebar, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			ImGui::TextWrapped(hovered->requirements);
			ImGui::End();
		}
		
		if (show_trophies == 2) {
			char titlebar[256];
			sprintf(titlebar, "%s Trophies (%s to close)", hovered->name, SCE_CTRL_CANCEL == SCE_CTRL_CIRCLE ? "Circle" : "Cross");
			ImGui::SetNextWindowPos(ImVec2(80, 55), ImGuiSetCond_Always);
			ImGui::SetNextWindowSize(ImVec2(800, 472), ImGuiSetCond_Always);
			ImGui::Begin(titlebar, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
			TrophySelection *t = trophies;
			int trp_id = 0;
			while (t) {
				char label[512];
				sprintf(label, "%s\n%s ##%d", t->name, t->desc, trp_id++);
				float y = ImGui::GetCursorPosY() + 2.0f;
				ImGui::Button(label, ImVec2(-1.0f, 0.0f));
				ImGui::SetCursorPos(ImVec2(735.0f, y));
				ImGui::Image((void*)t->icon, ImVec2(35, 35));
				t = t->next;
			}
			ImGui::PopStyleVar();
			ImGui::End();
		}
		
		glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
		ImGui::Render();
		ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
		
		vglSwapBuffers(GL_FALSE);
		
		// Extra controls handling
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_LTRIGGER && !(oldpad & SCE_CTRL_LTRIGGER) && !show_trailer && !show_screenshots && !show_changelog && !show_requirements && !show_trophies && !extra_menu_invoked) {
			calculate_right_len = true;
			old_sort_idx = -1;
			sort_idx = 0;
			filter_idx = 0;
			mode_idx = (mode_idx + 1) % MODES_NUM;
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_RTRIGGER && !(oldpad & SCE_CTRL_RTRIGGER) && !show_trailer && !show_screenshots && !show_changelog && !show_requirements && !show_trophies && !extra_menu_invoked) {
			if (mode_idx == MODE_THEMES)
				sort_idx = (sort_idx + 1) % (sizeof(sort_modes_themes_str) / sizeof(sort_modes_themes_str[0]));
			else
				sort_idx = (sort_idx + 1) % (sizeof(sort_modes_str) / sizeof(sort_modes_str[0]));
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_START && !(oldpad & SCE_CTRL_START) && hovered && (strlen(hovered->screenshots) > 5 || mode_idx == MODE_THEMES || strlen(hovered->trailer) > 5) && !show_changelog && !show_requirements && !show_trophies) {
			if (mode_idx == MODE_THEMES) {
				show_screenshots = show_screenshots ? 0 : 1;
			} else if (strlen(hovered->trailer) > 5) {
				if (show_screenshots) {
					show_screenshots = 0;
				} else {
					show_trailer = show_trailer ? 0 : 1;
					if (!show_trailer) {
						video_close();
						if (has_animated_bg) {
							LoadBackground();
						}
						audio_thd = sceKernelCreateThread("Audio Playback", &musicThread, 0x10000100, 0x100000, 0, 0, NULL);
						sceKernelStartThread(audio_thd, 0, NULL);
					}
				}
			} else {
				show_screenshots = show_screenshots ? 0 : 1;
			}
		} else if (pad.buttons & SCE_CTRL_SELECT && !(oldpad & SCE_CTRL_SELECT) && !show_trailer && !show_screenshots && !show_changelog && !show_requirements && !show_trophies) {
			if (mode_idx == MODE_THEMES) {
				shuffle_themes = !shuffle_themes;
				ThemeSelection *g = themes;
				FILE *f = fopen("ux0:data/VitaDB/shuffle.cfg", "w");
				bool has_shuffle = false;
				while (g) {
					if (g->shuffle) {
						has_shuffle = true;
						fprintf(f, "%s\n", g->name);
						g->shuffle = false;
					}
					g = g->next;
				}
				fclose(f);
				if (has_shuffle) {
					install_theme_from_shuffle(false);
				} else
					sceIoRemove("ux0:data/VitaDB/shuffle.cfg");
			} else if (hovered)
				extra_menu_invoked = !extra_menu_invoked;
		} else if (pad.buttons & SCE_CTRL_LEFT && !(oldpad & SCE_CTRL_LEFT) && !show_trailer && !show_changelog && !show_requirements && !show_trophies && (!extra_menu_invoked || show_screenshots)) {
			if (show_screenshots)
				cur_ss_idx--;
			else {
				fast_decrement = true;
				decrement_stack_idx = 0;
				decremented_app = nullptr;
			}
		} else if (pad.buttons & SCE_CTRL_RIGHT && !(oldpad & SCE_CTRL_RIGHT) && !show_trailer && !show_changelog && !show_requirements && !show_trophies && (!extra_menu_invoked || show_screenshots)) {
			if (show_screenshots)
				cur_ss_idx++;
			else
				fast_increment = true;
		} else if (pad.buttons & SCE_CTRL_CANCEL && !(oldpad & SCE_CTRL_CANCEL)) {
			if (show_changelog) {
				show_changelog = false;
				free(changelog);
			} else if (show_requirements) {
				show_requirements = false;
			} else if (show_trophies) {
				early_stop_trp_icon_upload = 1;
				sceKernelWaitThreadEnd(trophy_thd, NULL, NULL);
				early_stop_trp_icon_upload = 0;
				show_trophies = 0;
				while (trophies) {
					TrophySelection *t = trophies;
					free(t->desc);
					if (t->icon != empty_icon)
						glDeleteTextures(1, &t->icon);
					trophies = trophies->next;
					free(t);
				}
			} else if (show_screenshots) {
				show_screenshots = 0;
			} else if (show_trailer) {
				video_close();
				if (has_animated_bg) {
					LoadBackground();
				}
				audio_thd = sceKernelCreateThread("Audio Playback", &musicThread, 0x10000100, 0x100000, 0, 0, NULL);
				sceKernelStartThread(audio_thd, 0, NULL);
				show_trailer = 0;
			} else if (extra_menu_invoked) {
				extra_menu_invoked = false;
			} else
				go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_TRIANGLE && !(oldpad & SCE_CTRL_TRIANGLE) && !show_trailer && !show_screenshots && !show_requirements && !show_trophies && !show_changelog && !extra_menu_invoked) {
			init_interactive_ime_dialog("Insert search term", app_name_filter);
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_SQUARE && !(oldpad & SCE_CTRL_SQUARE) && !show_trailer && !show_screenshots && !show_requirements && !show_trophies && !show_changelog && !extra_menu_invoked) {
			if (mode_idx == MODE_THEMES)
				filter_idx = (filter_idx + 1) % (sizeof(filter_themes_modes) / sizeof(*filter_themes_modes));
			else
				filter_idx = (filter_idx + 1) % (sizeof(filter_modes) / sizeof(*filter_modes));
			go_to_top = true;
		}
		oldpad = pad.buttons;
		bool anti_burn_in_set_up = false;
		
		// Queued app uninstall
		if (to_uninstall) {
			if (!anti_burn_in_set_up)
				anti_burn_in_set_up = prepare_anti_burn_in();
			init_interactive_msg_dialog("Do you really wish to uninstall this app?");
			while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
				draw_simple_texture(previous_frame);
				vglSwapBuffers(GL_TRUE);
			}
			SceMsgDialogResult msg_res;
			memset(&msg_res, 0, sizeof(SceMsgDialogResult));
			sceMsgDialogGetResult(&msg_res);
			sceMsgDialogTerm();
			if (msg_res.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
				if (mode_idx == MODE_VITA_HBS) {
					scePromoterUtilInit();
					scePromoterUtilityDeletePkg(to_uninstall->titleid);
					int state = 0;
					do {
						int ret = scePromoterUtilityGetState(&state);
						if (ret < 0)
							break;
						draw_text_dialog("Uninstalling the app", true, false);
						vglSwapBuffers(GL_TRUE);
					} while (state);
					scePromoterUtilTerm();
				} else {
					char tmp_path[256];
					sprintf(tmp_path, "%spspemu/PSP/GAME/%s", pspemu_dev, to_uninstall->id);
					recursive_rmdir(tmp_path);
				}
				to_uninstall->state = APP_UNTRACKED;
			}
			to_uninstall = nullptr;
		}
		
		// Queued app download
		if (to_download) {
			if (mode_idx == MODE_THEMES) {
				if (!anti_burn_in_set_up)
						anti_burn_in_set_up = prepare_anti_burn_in();
				ThemeSelection *node = (ThemeSelection *)to_download;
				sprintf(download_link, "https://github.com/CatoTheYounger97/vitaDB_themes/blob/main/themes/%s/theme.zip?raw=true", node->name);
				download_file(download_link, "Downloading theme", false, -1, -1, previous_frame);
				sprintf(download_link, "ux0:data/VitaDB/themes/%s/", node->name);
				sceIoMkdir(download_link, 0777);
				extract_zip_file(TEMP_DOWNLOAD_NAME, download_link, false);
				sceIoRemove(TEMP_DOWNLOAD_NAME);
				node->state = APP_UPDATED;
				to_download = nullptr;
			} else {
				if (to_download->requirements && ((!strstr(to_download->requirements, "libshacccg.suprx")) || strlen(to_download->requirements) != strlen("- libshacccg.suprx"))) {
					if (!anti_burn_in_set_up)
						anti_burn_in_set_up = prepare_anti_burn_in();
					init_interactive_msg_dialog("This homebrew has extra requirements in order to work properly:\n%s\n\nDo you wish to install it still?", to_download->requirements);
					while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
						draw_simple_texture(previous_frame);
						vglSwapBuffers(GL_TRUE);
					}
					SceMsgDialogResult msg_res;
					memset(&msg_res, 0, sizeof(SceMsgDialogResult));
					sceMsgDialogGetResult(&msg_res);
					sceMsgDialogTerm();
					if (msg_res.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
						if (strstr(to_download->requirements, "kubridge.skprx")) {
							if (kubridge_state != KUBRIDGE_MISSING) {
								char cur_hash[40];
								SceUID f = sceIoOpen(kubridge_state == KUBRIDGE_UR0 ? "ur0:tai/kubridge.skprx" : "ux0:tai/kubridge.skprx", SCE_O_RDONLY, 0777);
								calculate_md5(f, cur_hash);
								silent_download("https://www.rinnegatamante.eu/vitadb/get_hb_hash.php?id=611");
								if (strncmp(cur_hash, (const char *)generic_mem_buffer, 32)) {
									init_interactive_msg_dialog("VitaDB Downloader detected an outdated version of kubridge.skprx. Do you wish to update it?\n\nNOTE: A console restart is required for kubridge.skprx update to complete.");
									while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
										draw_simple_texture(previous_frame);
										vglSwapBuffers(GL_TRUE);
									}
									SceMsgDialogResult msg_res;
									memset(&msg_res, 0, sizeof(SceMsgDialogResult));
									sceMsgDialogGetResult(&msg_res);
									sceMsgDialogTerm();
									if (msg_res.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
										download_file("https://www.rinnegatamante.eu/vitadb/get_hb_url.php?id=611", "Downloading kubridge.skprx", false, -1, -1, previous_frame);
										if (kubridge_state == KUBRIDGE_UR0) {
											copy_file(TEMP_DOWNLOAD_NAME, "ur0:tai/kubridge.skprx");
											sceIoRemove(TEMP_DOWNLOAD_NAME);
										} else {
											sceIoRemove("ux0:tai/kubridge.skprx");
											sceIoRename(TEMP_DOWNLOAD_NAME, "ux0:tai/kubridge.skprx");
										}
									}
								}
							} else {
								init_interactive_msg_dialog("This homebrew requires kubridge.skprx but it's not installed on this console. Do you wish to install it as well?\n\nNOTE: A console restart is required for kubridge.skprx installation to complete.");
								while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
									draw_simple_texture(previous_frame);
									vglSwapBuffers(GL_TRUE);
								}
								SceMsgDialogResult msg_res;
								memset(&msg_res, 0, sizeof(SceMsgDialogResult));
								sceMsgDialogGetResult(&msg_res);
								sceMsgDialogTerm();
								if (msg_res.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
									download_file("https://www.rinnegatamante.eu/vitadb/get_hb_url.php?id=611", "Downloading kubridge.skprx", false, -1, -1, previous_frame);
									if (use_ur0_config) {
										copy_file(TEMP_DOWNLOAD_NAME, "ur0:tai/kubridge.skprx");
										sceIoRemove(TEMP_DOWNLOAD_NAME);
										SceUID f = sceIoOpen("ur0:tai/config.txt", SCE_O_RDONLY, 0777);
										size_t len = sceIoRead(f, generic_mem_buffer, MEM_BUFFER_SIZE);
										sceIoClose(f);
										sprintf(user_plugin_str, "*KERNEL\nur0:tai/kubridge.skprx\n");
										f = sceIoOpen("ur0:tai/config.txt", SCE_O_CREAT | SCE_O_TRUNC | SCE_O_WRONLY, 0777);
										sceIoWrite(f, user_plugin_str, strlen(user_plugin_str));
										sceIoWrite(f, generic_mem_buffer, len);
										sceIoClose(f);
										kubridge_state = KUBRIDGE_UR0;
									} else {
										sceIoRemove("ux0:tai/kubridge.skprx"); // Just to be safe
										sceIoRename(TEMP_DOWNLOAD_NAME, "ux0:tai/kubridge.skprx");
										SceUID f = sceIoOpen("ux0:tai/config.txt", SCE_O_RDONLY, 0777);
										size_t len = sceIoRead(f, generic_mem_buffer, MEM_BUFFER_SIZE);
										sceIoClose(f);
										sprintf(user_plugin_str, "*KERNEL\nux0:tai/kubridge.skprx\n");
										f = sceIoOpen("ux0:tai/config.txt", SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0777);
										sceIoWrite(f, user_plugin_str, strlen(user_plugin_str));
										sceIoWrite(f, generic_mem_buffer, len);
										sceIoClose(f);
										kubridge_state = KUBRIDGE_UX0;
									}
								}
							}
						}
					} else {
						to_download = nullptr;
						goto skip_install;
					}
				}
				
				if (to_download->prev_clash || to_download->next_clash) {
					if (to_download->state != APP_UNTRACKED) {
						AppSelection *installed_clasher = nullptr;
						AppSelection *chk = to_download->prev_clash;
						while (chk) {
							if (chk->state == APP_UPDATED) {
								installed_clasher = chk;
								break;
							}
							chk = chk->prev_clash;
						}
						if (!installed_clasher) {
							chk = to_download->next_clash;
							while (chk) {
								if (chk->state == APP_UPDATED) {
									installed_clasher = chk;
									break;
								}
								chk = chk->next_clash;
							}
						}
						if (!anti_burn_in_set_up)
							anti_burn_in_set_up = prepare_anti_burn_in();
						char clash_text[512];
						if (installed_clasher)
							sprintf(clash_text, "This homebrew has a TitleID that clashes with other homebrews. Installing it will automatically uninstall any homebrew you have installed with the same TitleID.\nVitaDB Downloader detected this app as the installed one with clashing TitleID:\n%s\nDo you want to proceed?", installed_clasher->name);
						else
							sprintf(clash_text, "This homebrew has a TitleID that clashes with other homebrews. Installing it will automatically uninstall any homebrew you have installed with the same TitleID.\nDo you want to proceed?");
						init_interactive_msg_dialog(clash_text);
						while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
							draw_simple_texture(previous_frame);
							vglSwapBuffers(GL_TRUE);
						}
						SceMsgDialogResult msg_res;
						memset(&msg_res, 0, sizeof(SceMsgDialogResult));
						sceMsgDialogGetResult(&msg_res);
						sceMsgDialogTerm();
						if (msg_res.buttonId != SCE_MSG_DIALOG_BUTTON_ID_YES) {
							to_download = nullptr;
							continue;
						}
					}
				}
				
				bool downloading_data_files = false;
				if (strlen(to_download->data_link) > 5) {
					if (!anti_burn_in_set_up)
						anti_burn_in_set_up = prepare_anti_burn_in();
					init_interactive_msg_dialog("This homebrew also has data files. Do you wish to install them as well?");
					while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
						draw_simple_texture(previous_frame);
						vglSwapBuffers(GL_TRUE);
					}
					SceMsgDialogResult msg_res;
					memset(&msg_res, 0, sizeof(SceMsgDialogResult));
					sceMsgDialogGetResult(&msg_res);
					sceMsgDialogTerm();
					if (msg_res.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
						downloading_data_files = true;
						// Check if enough storage is left for the install
						char *dummy;
						uint64_t sz = strtoull(to_download->size, &dummy, 10);
						uint64_t sz2 = strtoull(to_download->data_size, &dummy, 10);
						if (free_space < (sz + sz2) * 2) {
							init_msg_dialog("Not enough free storage to install this application. Installation aborted.");
							while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
								draw_simple_texture(previous_frame);
								vglSwapBuffers(GL_TRUE);
							}
							to_download = nullptr;
							sceMsgDialogTerm();
							continue;
						}
						if (!download_file(to_download->data_link, "Downloading data files", true, -1, -1, previous_frame)) {
							to_download = nullptr;
							sceIoRemove(TEMP_DOWNLOAD_NAME);
							continue;
						}
						if (mode_idx == MODE_VITA_HBS) {
							sceIoMkdir(TEMP_DATA_DIR, 0777);
							// Some Vita homebrews are not hosted on VitaDB webhost, thus lacking PSARC, so we need to check what is what we got
							uint32_t header;
							SceUID f = sceIoOpen(TEMP_DOWNLOAD_NAME, SCE_O_RDONLY, 0777);
							sceIoRead(f, &header, 4);
							sceIoClose(f);
							bool extract_finished;
							if (header == 0x52415350) // PSARC
								extract_finished = extract_psarc_file(TEMP_DOWNLOAD_NAME, TEMP_DATA_DIR, true, previous_frame);
							else // ZIP
								extract_finished = extract_zip_file(TEMP_DOWNLOAD_NAME, TEMP_DATA_PATH, false, true);
							if (!extract_finished) {
								sceIoRemove(TEMP_DOWNLOAD_NAME);
								recursive_rmdir(TEMP_DATA_DIR);
								to_download = nullptr;
								continue;
							}
						} /* else {
							// This should never happen since PSP homebrews are self contained
							char tmp[16];
							sprintf(tmp, "%spspemu/", pspemu_dev);
							extract_zip_file(TEMP_DOWNLOAD_NAME, tmp, false);
						} */
						sceIoRemove(TEMP_DOWNLOAD_NAME);
					}
				}
				if (!anti_burn_in_set_up)
					anti_burn_in_set_up = prepare_anti_burn_in();
				// Check if enough storage is left for the install
				char *dummy;
				uint64_t sz = strtoull(to_download->size, &dummy, 10);
				if (free_space < sz * 2) {
					init_msg_dialog("Not enough free storage to install this application. Installation aborted.");
					while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
						draw_simple_texture(previous_frame);
						vglSwapBuffers(GL_TRUE);
					}
					sceMsgDialogTerm();
					to_download = nullptr;
					continue;
				}
				sprintf(download_link, "https://www.rinnegatamante.eu/vitadb/get_psarc_url.php?id=%s", to_download->id);
				if (!download_file(download_link, (char *)(mode_idx == MODE_VITA_HBS ? "Downloading vpk" : "Downloading app"), true, -1, -1, previous_frame)) {
					to_download = nullptr;
					sceIoRemove(TEMP_DOWNLOAD_NAME);
					if (downloading_data_files)
						recursive_rmdir(TEMP_DATA_DIR);
					continue;
				}
				if (!strncmp(to_download->id, "877", 3)) { // Updating VitaDB Downloader
					extract_psarc_file(TEMP_DOWNLOAD_NAME, "ux0:app/VITADBDLD", false, previous_frame); // We don't want VitaDB Downloader update to be abortable to prevent corruption
					sceIoRemove(TEMP_DOWNLOAD_NAME);
					SceUID f = sceIoOpen("ux0:app/VITADBDLD/hash.vdb", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
					sceIoWrite(f, to_download->hash, 32);
					sceIoClose(f);
					sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
				} else {
					SceUID f;
					char tmp_path[256];
					if (mode_idx == MODE_VITA_HBS) {
						// Some Vita homebrews are not hosted on VitaDB webhost, thus lacking PSARC, so we need to check what is what we got
						uint32_t header;
						f = sceIoOpen(TEMP_DOWNLOAD_NAME, SCE_O_RDONLY, 0777);
						sceIoRead(f, &header, 4);
						sceIoClose(f);
						sceIoMkdir(TEMP_INSTALL_DIR, 0777);
						bool extract_finished;
						if (header == 0x52415350) // PSARC
							extract_finished = extract_psarc_file(TEMP_DOWNLOAD_NAME, TEMP_INSTALL_DIR, true, previous_frame);
						else // ZIP
							extract_finished = extract_zip_file(TEMP_DOWNLOAD_NAME, TEMP_INSTALL_PATH, false, true);
						sceIoRemove(TEMP_DOWNLOAD_NAME);
						if (!extract_finished) {
							if (downloading_data_files)
								recursive_rmdir(TEMP_DATA_DIR);
							recursive_rmdir(TEMP_INSTALL_DIR);
							to_download = nullptr;
							continue;
						}
						if (strlen(to_download->aux_hash) > 0) {
							f = sceIoOpen(AUX_HASH_FILE, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
							sceIoWrite(f, to_download->aux_hash, 32);
							sceIoClose(f);
						}
						f = sceIoOpen(HASH_FILE, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
					} else {
						sprintf(tmp_path, "%spspemu/PSP/GAME/%s", pspemu_dev, to_download->id);
						sceIoMkdir(tmp_path, 0777);
						bool extract_finished = extract_psarc_file(TEMP_DOWNLOAD_NAME, tmp_path, true, previous_frame);
						sceIoRemove(TEMP_DOWNLOAD_NAME);
						if (!extract_finished) {
							recursive_rmdir(tmp_path);
							to_download = nullptr;
							continue;
						}
						sprintf(tmp_path, "%spspemu/PSP/GAME/%s/hash.vdb", pspemu_dev, to_download->id);
						f = sceIoOpen(tmp_path, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
					}
					sceIoWrite(f, to_download->hash, 32);
					sceIoClose(f);
					if (mode_idx == MODE_VITA_HBS) {
						makeHeadBin(TEMP_INSTALL_DIR);
						scePromoterUtilInit();
						scePromoterUtilityPromotePkg(TEMP_INSTALL_DIR, 0);
						int state = 0;
						do {
							int ret = scePromoterUtilityGetState(&state);
							if (ret < 0)
								break;
							draw_text_dialog("Installing the app", true, false);
							vglSwapBuffers(GL_TRUE);
						} while (state);
						scePromoterUtilTerm();
						if (sceIoGetstat(TEMP_INSTALL_DIR, &st) >= 0) {
							init_msg_dialog("The installation process failed.");
							while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
								draw_simple_texture(previous_frame);
								vglSwapBuffers(GL_TRUE);
							}
							sceMsgDialogTerm();
							recursive_rmdir(TEMP_INSTALL_DIR);
							if (downloading_data_files)
								recursive_rmdir(TEMP_DATA_DIR);
						} else {
							to_download->state = APP_UPDATED;
							if (downloading_data_files)
								move_path(TEMP_DATA_DIR, "ux0:data");
						}
					} else
						to_download->state = APP_UPDATED;
					to_download = nullptr;
				}
			}
		}
skip_install:		
		// Ime dialog active
		if (is_ime_active) {
			if (!anti_burn_in_set_up)
				anti_burn_in_set_up = prepare_anti_burn_in();
			while (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
				draw_simple_texture(previous_frame);
				vglSwapBuffers(GL_TRUE);
			}
			SceImeDialogResult res;
			sceClibMemset(&res, 0, sizeof(SceImeDialogResult));
			sceImeDialogGetResult(&res);
			if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
				get_dialog_text_result(app_name_filter);
			}
			sceImeDialogTerm();
			is_ime_active = false;
		}
		
		// Queued trophies download
		if (show_trophies == 1) {
			if (!anti_burn_in_set_up)
				anti_burn_in_set_up = prepare_anti_burn_in();
			char dl_url[512];
			sprintf(dl_url, "ux0:data/VitaDB/trophies/%s", hovered->titleid);
			sceIoMkdir(dl_url, 0777);
			sprintf(dl_url, "https://www.rinnegatamante.eu/vitadb/get_trophies_for_app.php?id=%s", hovered->titleid);
			download_file(dl_url, "Downloading trophies data", false, -1, -1, previous_frame);
			SceUID f = sceIoOpen(TEMP_DOWNLOAD_NAME, SCE_O_RDONLY, 0777);
			size_t sz = sceIoLseek(f, 0, SCE_SEEK_END);
			sceIoLseek(f, 0, SCE_SEEK_SET);
			char *trp_data = (char *)malloc(sz + 1);
			sceIoRead(f, trp_data, sz);
			trp_data[sz] = 0;
			sceIoClose(f);
			sceIoRemove(TEMP_DOWNLOAD_NAME);
			char *s = strstr(trp_data, "\"name\":");
			TrophySelection *last_trp = nullptr;
			trophies = nullptr;
			int trp_count = 0;
			while (s) {
				TrophySelection *trp = (TrophySelection *)malloc(sizeof(TrophySelection));
				if (trophies == nullptr)
					trophies = trp;
				s += 9;
				char *end = strstr(s, "\"");
				sceClibMemcpy(trp->name, s, end - s);
				trp->name[end - s] = 0;
				s = strstr(end, "\"desc\":") + 9;
				end = strstr(s, "\",\n");
				trp->desc = (char *)malloc(end - s + 1);
				sceClibMemcpy(trp->desc, s, end - s);
				trp->desc[end - s] = 0;
				trp->desc = unescape(trp->desc);
				s = strstr(end, "\"icon\":") + 9;
				end = strstr(s, "\"");
				sceClibMemcpy(trp->icon_name, s, end - s);
				trp->icon_name[end - s] = 0;
				trp->icon = empty_icon;
				strcpy(trp->titleid, hovered->titleid);
				trp->next = nullptr;
				if (last_trp)
					last_trp->next = trp;
				last_trp = trp;
				s = strstr(end, "\"name\":");
				trp_count++;
			}
			TrophySelection *trp = trophies;
			int trp_index = 0;
			while (trp) {
				PrepareTrophy(hovered->titleid, trp->icon_name, trp_index++, trp_count);
				trp = trp->next;
			}
			trophy_thd = sceKernelCreateThread("Trophies Loader", &trophy_loader, 0x10000100, 0x100000, 0, 0, NULL);
			sceKernelStartThread(trophy_thd, 0, NULL);
			free(trp_data);
			show_trophies = 2;
		}
		
		// Queued trailer request
		if (show_trailer == 1) {
			if (has_animated_bg) {
				video_close();
			}
			
			// Kill old audio playback
			SceKernelThreadInfo info;
			info.size = sizeof(SceKernelThreadInfo);
			int res = sceKernelGetThreadInfo(audio_thd, &info);
			if (res >= 0) {
				kill_audio_thread = true;
				while (sceKernelGetThreadInfo(audio_thd, &info) >= 0) {
					sceKernelDelayThread(1000);
				}
			}
			
			// Start trailer streaming
			char trailer_url[256];
			sprintf(trailer_url, "https://www.rinnegatamante.eu/vitadb/videos/%s.mp4", hovered->trailer);
			video_open(trailer_url);
			show_trailer = 2;
		}
		
		// Queued screenshots download
		if (show_screenshots == 1) {
			if (!anti_burn_in_set_up)
				anti_burn_in_set_up = prepare_anti_burn_in();
			sceIoRemove("ux0:data/VitaDB/ss0.png");
			sceIoRemove("ux0:data/VitaDB/ss1.png");
			sceIoRemove("ux0:data/VitaDB/ss2.png");
			sceIoRemove("ux0:data/VitaDB/ss3.png");
			old_ss_idx = -1;
			cur_ss_idx = 0;
			int shot_idx = 0;
			if (mode_idx == MODE_THEMES) {
				ThemeSelection *node = (ThemeSelection *)hovered;
				sprintf(download_link, "https://github.com/CatoTheYounger97/vitaDB_themes/raw/main/themes/%s/preview.png", node->name);				
				download_file(download_link, "Downloading screenshot", false, -1, -1, previous_frame);
				sceIoRename(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/ss0.png");
			} else {
				char *s = hovered->screenshots;
				char shot_links[4][256];
				int shot_num = 0;
				for (;;) {
					char *end = strstr(s, ";");
					if (end) {
						end[0] = 0;
					}
					sprintf(shot_links[shot_num++], "https://www.rinnegatamante.eu/vitadb/%s", s);
					if (end) {
						end[0] = ';';
						s = end + 1;
					} else {
						break;
					}
				}
				while (shot_idx < shot_num) {
					download_file(shot_links[shot_idx], "Downloading screenshot", false, shot_idx + 1, shot_num, previous_frame);
					sprintf(shot_links[shot_idx], "ux0:data/VitaDB/ss%d.png", shot_idx);
					sceIoRename(TEMP_DOWNLOAD_NAME, shot_links[shot_idx++]);
				}
			}
			show_screenshots = 2;
		}
		
		// Queued theme to install
		if (to_install) {
			sceIoRemove("ux0:data/VitaDB/shuffle.cfg");
			install_theme(to_install);
			to_install = nullptr;
		}
		
		// Queued themes database download
		if (mode_idx == MODE_THEMES && !themes) {
			if (!anti_burn_in_set_up)
				anti_burn_in_set_up = prepare_anti_burn_in();
			if (sceIoGetstat("ux0:/data/VitaDB/previews", &st) < 0) {
				download_file("https://github.com/CatoTheYounger97/vitaDB_themes/releases/download/Nightly/previews.zip", "Downloading themes previews", false, -1, -1, previous_frame);
				extract_zip_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/", false);
			}
			
			download_file("https://github.com/CatoTheYounger97/vitaDB_themes/releases/download/Nightly/themes.json", "Downloading themes list", false, -1, -1, previous_frame);
			sceIoRemove("ux0:data/VitaDB/themes.json");
			sceIoRename(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/themes.json");
			populate_themes_database("ux0:data/VitaDB/themes.json");
		}
		
		// PSP database update required
		if (mode_idx == MODE_PSP_HBS && !psp_apps) {
			if (!anti_burn_in_set_up)
				anti_burn_in_set_up = prepare_anti_burn_in();
			SceUID thd = sceKernelCreateThread("Apps List Downloader", &appPspListThread, 0x10000100, 0x100000, 0, 0, NULL);
			sceKernelStartThread(thd, 0, NULL);
			do {
				draw_downloader_dialog(downloader_pass, downloaded_bytes, total_bytes, "Downloading PSP apps list", 1, true, previous_frame);
				res = sceKernelGetThreadInfo(thd, &info);
			} while (info.status <= SCE_THREAD_DORMANT && res >= 0);
			populate_apps_database("ux0:data/VitaDB/psp_apps.json", true);
		}
	}

	// Installing update
	if (to_download) {
		if (strlen(boot_params) > 0) { // On-demand app updater
			SceUID f;
			char hb_url[256], hb_message[256];
			sprintf(hb_url, "https://www.rinnegatamante.eu/vitadb/get_hb_url.php?id=%s", boot_params);
			sprintf(hb_message, "Downloading %s", to_download->name);
			download_file(hb_url, hb_message);
			sceIoMkdir(TEMP_INSTALL_DIR, 0777);
			extract_zip_file(TEMP_DOWNLOAD_NAME, TEMP_INSTALL_PATH, false);
			sceIoRemove(TEMP_DOWNLOAD_NAME);
			if (strlen(to_download->aux_hash) > 0) {
				f = sceIoOpen(AUX_HASH_FILE, SCE_O_CREAT | SCE_O_TRUNC | SCE_O_WRONLY, 0777);
				sceIoWrite(f, to_download->aux_hash, 32);
				sceIoClose(f);
			}
			f = sceIoOpen(HASH_FILE, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
			sceIoWrite(f, to_download->hash, 32);
			sceIoClose(f);
			makeHeadBin(TEMP_INSTALL_DIR);
			scePromoterUtilInit();
			scePromoterUtilityPromotePkg(TEMP_INSTALL_DIR, 0);
			int state = 0;
			do {
				int ret = scePromoterUtilityGetState(&state);
				if (ret < 0)
					break;
				draw_text_dialog("Installing the update", true, false);
				vglSwapBuffers(GL_TRUE);
			} while (state);
			scePromoterUtilTerm();
			if (sceIoGetstat(TEMP_INSTALL_DIR, &st) >= 0) {
				init_msg_dialog("The installation process failed.");
				while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
					draw_simple_texture(previous_frame);
					vglSwapBuffers(GL_TRUE);
				}
				sceMsgDialogTerm();
				recursive_rmdir(TEMP_INSTALL_DIR);
			}
		} else { // VitaDB Downloader auto-updater
			download_file("https://www.rinnegatamante.eu/vitadb/get_psarc_url.php?id=877", "Downloading update");
			extract_psarc_file(TEMP_DOWNLOAD_NAME, "ux0:app/VITADBDLD", false);
			sceIoRemove(TEMP_DOWNLOAD_NAME);
			SceUID f = sceIoOpen("ux0:app/VITADBDLD/hash.vdb", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
			sceIoWrite(f, to_download->hash, 32);
			sceIoClose(f);
			sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
		}
	} else {
		init_msg_dialog("This homebrew is already up to date.");
		while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
			vglSwapBuffers(GL_TRUE);
		}
		sceMsgDialogTerm();
	}
}
