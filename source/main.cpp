#include <iostream>
#include <string>
#include <locale>
#include <codecvt>
#include <vitasdk.h>
#include <vitaGL.h>
#include <imgui_vita.h>
#include <imgui_internal.h>
#include <bzlib.h>
#include <curl/curl.h>
#include <stdio.h>
#include <string>
#include <soloud.h>
#include <soloud_wav.h>
#include "head.h"
#include "unzip.h"
#include "sha1.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define MEM_BUFFER_SIZE (32 * 1024 * 1024)
#define SCR_WIDTH 960
#define SCR_HEIGHT 544
#define VERSION "1.2"
#define TEMP_DOWNLOAD_NAME "ux0:data/VitaDB/temp.tmp"
#define MIN(x, y) (x) < (y) ? (x) : (y)
#define PREVIEW_PADDING 6
#define PREVIEW_HEIGHT 128.0f
#define PREVIEW_WIDTH  128.0f

int _newlib_heap_size_user = 200 * 1024 * 1024;
int console_language;

typedef struct{
	uint32_t magic;
	uint32_t version;
	uint32_t keyTableOffset;
	uint32_t dataTableOffset;
	uint32_t indexTableEntries;
} sfo_header_t;

typedef struct{
	uint16_t keyOffset;
	uint16_t param_fmt;
	uint32_t paramLen;
	uint32_t paramMaxLen;
	uint32_t dataOffset;
} sfo_entry_t;

// Taken from VHBB, thanks to devnoname120
static void fpkg_hmac(const uint8_t* data, unsigned int len, uint8_t hmac[16]) {
	SHA1_CTX ctx;
	char sha1[20];
	char buf[64];

	sha1_init(&ctx);
	sha1_update(&ctx, (BYTE*)data, len);
	sha1_final(&ctx, (BYTE*)sha1);

	sceClibMemset(buf, 0, 64);
	sceClibMemcpy(&buf[0], &sha1[4], 8);
	sceClibMemcpy(&buf[8], &sha1[4], 8);
	sceClibMemcpy(&buf[16], &sha1[12], 4);
	buf[20] = sha1[16];
	buf[21] = sha1[1];
	buf[22] = sha1[2];
	buf[23] = sha1[3];
	sceClibMemcpy(&buf[24], &buf[16], 8);

	sha1_init(&ctx);
	sha1_update(&ctx, (BYTE*)buf, 64);
	sha1_final(&ctx, (BYTE*)sha1);
	sceClibMemcpy(hmac, sha1, 16);
}
void makeHeadBin(const char *dir) {
	uint8_t hmac[16];
	uint32_t off;
	uint32_t len;
	uint32_t out;

	char head_path[256];
	char param_path[256];
	sprintf(head_path, "%s/sce_sys/package/head.bin", dir);
	sprintf(param_path, "%s/sce_sys/param.sfo", dir);
	
	SceUID fileHandle = sceIoOpen(head_path, SCE_O_RDONLY, 0777);
	if (fileHandle >= 0) {
		sceIoClose(fileHandle);
		return;
	}

	FILE* f = fopen(param_path,"rb");
	
	if (f == NULL)
		return;
	
	sfo_header_t hdr;
	fread(&hdr, sizeof(sfo_header_t), 1, f);
	
	if (hdr.magic != 0x46535000){
		fclose(f);
		return;
	}
	
	uint8_t* idx_table = (uint8_t*)malloc((sizeof(sfo_entry_t)*hdr.indexTableEntries));
	fread(idx_table, sizeof(sfo_entry_t)*hdr.indexTableEntries, 1, f);
	sfo_entry_t* entry_table = (sfo_entry_t*)idx_table;
	fseek(f, hdr.keyTableOffset, SEEK_SET);
	uint8_t* key_table = (uint8_t*)malloc(hdr.dataTableOffset - hdr.keyTableOffset);
	fread(key_table, hdr.dataTableOffset - hdr.keyTableOffset, 1, f);
	
	char titleid[12];
	char contentid[48];
	
	for (int i=0; i < hdr.indexTableEntries; i++) {
		char param_name[256];
		sprintf(param_name, "%s", (char*)&key_table[entry_table[i].keyOffset]);
			
		if (strcmp(param_name, "TITLE_ID") == 0){ // Application Title ID
			fseek(f, hdr.dataTableOffset + entry_table[i].dataOffset, SEEK_SET);
			fread(titleid, entry_table[i].paramLen, 1, f);
		} else if (strcmp(param_name, "CONTENT_ID") == 0) { // Application Content ID
			fseek(f, hdr.dataTableOffset + entry_table[i].dataOffset, SEEK_SET);
			fread(contentid, entry_table[i].paramLen, 1, f);
		}
	}

	// Free sfo buffer
	free(idx_table);
	free(key_table);

	// Allocate head.bin buffer
	uint8_t* head_bin = (uint8_t*)malloc(size_head);
	sceClibMemcpy(head_bin, head, size_head);

	// Write full title id
	char full_title_id[48];
	snprintf(full_title_id, sizeof(full_title_id), "EP9000-%s_00-0000000000000000", titleid);
	strncpy((char*)&head_bin[0x30], strlen(contentid) > 0 ? contentid : full_title_id, 48);

	// hmac of pkg header
	len = __builtin_bswap32(*(uint32_t*)&head_bin[0xD0]);
	fpkg_hmac(&head_bin[0], len, hmac);
	sceClibMemcpy(&head_bin[len], hmac, 16);

	// hmac of pkg info
	off = __builtin_bswap32(*(uint32_t*)&head_bin[0x8]);
	len = __builtin_bswap32(*(uint32_t*)&head_bin[0x10]);
	out = __builtin_bswap32(*(uint32_t*)&head_bin[0xD4]);
	fpkg_hmac(&head_bin[off], len - 64, hmac);
	sceClibMemcpy(&head_bin[out], hmac, 16);

	// hmac of everything
	len = __builtin_bswap32(*(uint32_t*)&head_bin[0xE8]);
	fpkg_hmac(&head_bin[0], len, hmac);
	sceClibMemcpy(&head_bin[len], hmac, 16);

	// Make dir
	char pkg_dir[256];
	sprintf(pkg_dir, "%s/sce_sys/package", dir);
	sceIoMkdir(pkg_dir, 0777);

	// Write head.bin
	fclose(f);
	f = fopen(head_path, "wb");
	fwrite(head_bin, 1, size_head, f);
	fclose(f);

	free(head_bin);
}

void recursive_mkdir(char *dir) {
	char *p = dir;
	while (p) {
		char *p2 = strstr(p, "/");
		if (p2) {
			p2[0] = 0;
			sceIoMkdir(dir, 0777);
			p = p2 + 1;
			p2[0] = '/';
		} else break;
	}
}

void early_fatal_error(const char *msg) {
	vglInit(0);
	SceMsgDialogUserMessageParam msg_param;
	sceClibMemset(&msg_param, 0, sizeof(SceMsgDialogUserMessageParam));
	msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
	msg_param.msg = (const SceChar8*)msg;
	SceMsgDialogParam param;
	sceMsgDialogParamInit(&param);
	param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
	param.userMsgParam = &msg_param;
	sceMsgDialogInit(&param);
	while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
		vglSwapBuffers(GL_TRUE);
	}
	sceKernelExitProcess(0);
}

int init_interactive_msg_dialog(const char *msg) {
	SceMsgDialogUserMessageParam msg_param;
	memset(&msg_param, 0, sizeof(msg_param));
	msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_YESNO;
	msg_param.msg = (SceChar8 *)msg;

	SceMsgDialogParam param;
	sceMsgDialogParamInit(&param);
	_sceCommonDialogSetMagicNumber(&param.commonParam);
	param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
	param.userMsgParam = &msg_param;

	return sceMsgDialogInit(&param);
}

static uint16_t dialog_res_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
bool is_ime_active = false;
void getDialogTextResult(char *text) {
	// Converting text from UTF16 to UTF8
	std::u16string utf16_str = (char16_t*)dialog_res_text;
	std::string utf8_str = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(utf16_str.data());
	strcpy(text, utf8_str.c_str());
}
void init_interactive_ime_dialog(const char *msg, const char *start_text) {
	SceImeDialogParam params;
	
	sceImeDialogParamInit(&params);
	params.type = SCE_IME_TYPE_BASIC_LATIN;
			
	// Converting texts from UTF8 to UTF16
	std::string utf8_str = msg;
	std::u16string utf16_str = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(utf8_str.data());
	std::string utf8_arg = start_text;
	std::u16string utf16_arg = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(utf8_arg.data());
			
	params.title = (const SceWChar16*)utf16_str.c_str();
	sceClibMemset(dialog_res_text, 0, sizeof(dialog_res_text));
	sceClibMemcpy(dialog_res_text, utf16_arg.c_str(), utf16_arg.length() * 2);
	params.initialText = dialog_res_text;
	params.inputTextBuffer = dialog_res_text;
			
	params.maxTextLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
			
	sceImeDialogInit(&params);
	is_ime_active = true;
}

static char *sizes[] = {
	"B",
	"KB",
	"MB",
	"GB"
};

static float format(float len) {
	while (len > 1024) len = len / 1024.0f;
	return len;
}

static uint8_t quota(uint64_t len) {
	uint8_t ret = 0;
	while (len > 1024) {
		ret++;
		len = len / 1024;
	}
	return ret;
}

void DrawExtractorDialog(int index, float file_extracted_bytes, float extracted_bytes, float file_total_bytes, float total_bytes, char *filename, int num_files) {
	ImGui_ImplVitaGL_NewFrame();
	
	char msg1[256], msg2[256];
	sprintf(msg1, "%s (%d / %d)", "Extracting archive...", index, num_files);
	sprintf(msg2, "%s (%.2f %s / %.2f %s)", filename, format(file_extracted_bytes), sizes[quota(file_extracted_bytes)], format(file_total_bytes), sizes[quota(file_total_bytes)]);
	ImVec2 pos1 = ImGui::CalcTextSize(msg1);
	ImVec2 pos2 = ImGui::CalcTextSize(msg2);
	
	ImGui::GetIO().MouseDrawCursor = false;
	ImGui::SetNextWindowPos(ImVec2((SCR_WIDTH / 2) - 200, (SCR_HEIGHT / 2) - 50), ImGuiSetCond_Always);
	ImGui::SetNextWindowSize(ImVec2(400, 100), ImGuiSetCond_Always);
	ImGui::Begin("", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
	ImGui::SetCursorPos(ImVec2((400 - pos1.x) / 2, 20));
	ImGui::Text(msg1);
	ImGui::SetCursorPos(ImVec2((400 - pos2.x) / 2, 40));
	ImGui::Text(msg2);
	ImGui::SetCursorPos(ImVec2(100, 60));
	ImGui::ProgressBar(extracted_bytes / total_bytes, ImVec2(200, 0));
	
	ImGui::End();
	glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
	ImGui::Render();
	ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
	vglSwapBuffers(GL_FALSE);
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
}

void DrawDownloaderDialog(int index, float downloaded_bytes, float total_bytes, char *text, int passes, bool self_contained) {
	sceKernelPowerTick(0);
	
	if (self_contained)
		ImGui_ImplVitaGL_NewFrame();
	
	char msg[512];
	sprintf(msg, "%s (%d / %d)", text, index, passes);
	ImVec2 pos = ImGui::CalcTextSize(msg);

	ImGui::SetNextWindowPos(ImVec2((SCR_WIDTH / 2) - 200, (SCR_HEIGHT / 2) - 50), ImGuiSetCond_Always);
	ImGui::SetNextWindowSize(ImVec2(400, 100), ImGuiSetCond_Always);
	ImGui::Begin("downloader", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
	
	ImGui::SetCursorPos(ImVec2((400 - pos.x) / 2, 20));
	ImGui::Text(msg);
	if (total_bytes < 4000000000.0f) {
		sprintf(msg, "%.2f %s / %.2f %s", format(downloaded_bytes), sizes[quota(downloaded_bytes)], format(total_bytes), sizes[quota(total_bytes)]);
		pos = ImGui::CalcTextSize(msg);
		ImGui::SetCursorPos(ImVec2((400 - pos.x) / 2, 40));
		ImGui::Text(msg);
		ImGui::SetCursorPos(ImVec2(100, 60));
		ImGui::ProgressBar(downloaded_bytes / total_bytes, ImVec2(200, 0));
	} else {
		sprintf(msg, "%.2f %s", format(downloaded_bytes), sizes[quota(downloaded_bytes)]);
		pos = ImGui::CalcTextSize(msg);
		ImGui::SetCursorPos(ImVec2((400 - pos.x) / 2, 50));
		ImGui::Text(msg);
	}
	
	ImGui::End();
	
	if (self_contained) {
		glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
		ImGui::Render();
		ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
		vglSwapBuffers(GL_FALSE);
	}
}

void DrawTextDialog(char *text, bool self_contained) {
	sceKernelPowerTick(0);
	
	if (self_contained)
		ImGui_ImplVitaGL_NewFrame();
	
	ImVec2 pos = ImGui::CalcTextSize(text);

	ImGui::SetNextWindowPos(ImVec2((SCR_WIDTH / 2) - 200, (SCR_HEIGHT / 2) - 50), ImGuiSetCond_Always);
	ImGui::SetNextWindowSize(ImVec2(400, 100), ImGuiSetCond_Always);
	ImGui::Begin("text dialog", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
	
	ImGui::SetCursorPos(ImVec2((400 - pos.x) / 2, 20));
	ImGui::Text(text);
	ImGui::End();
	
	if (self_contained) {
		glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
		ImGui::Render();
		ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
		vglSwapBuffers(GL_FALSE);
	}
}

int filter_idx = 0;
int cur_ss_idx;
int old_ss_idx = -1;
volatile char generic_url[512];
static char download_link[512];
static CURL *curl_handle = NULL;
static volatile uint64_t total_bytes = 0xFFFFFFFF;
static volatile uint64_t downloaded_bytes = 0;
static volatile uint8_t downloader_pass = 1;
uint8_t *generic_mem_buffer = nullptr;
static FILE *fh;
char *bytes_string;
char app_name_filter[128] = {0};

struct AppSelection {
	char name[192];
	char icon[128];
	char author[128];
	char type[2];
	char id[8];
	char date[12];
	char screenshots[512];
	char *desc;
	char downloads[16];
	char size[16];
	char data_size[16];
	char data_link[128];
	AppSelection *next;
};

static AppSelection *old_hovered = NULL;
AppSelection *apps = nullptr;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
	if (total_bytes > MEM_BUFFER_SIZE) {
		if (!fh)
			fh = fopen(TEMP_DOWNLOAD_NAME, "wb");
		fwrite(ptr, 1, nmemb, fh);
	} else {
		uint8_t *dst = &generic_mem_buffer[downloaded_bytes];
		sceClibMemcpy(dst, ptr, nmemb);
	}
	downloaded_bytes += nmemb;
	if (total_bytes < downloaded_bytes) total_bytes = downloaded_bytes;
	return nmemb;
}

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
	char *ptr = strcasestr(buffer, "Content-Length");
	if (ptr != NULL) sscanf(ptr, "Content-Length: %llu", &total_bytes);
	return nitems;
}

static void startDownload(const char *url) {
	curl_easy_reset(curl_handle);
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.36");
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl_handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
	curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, bytes_string); // Dummy
	curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, bytes_string); // Dummy
	curl_easy_setopt(curl_handle, CURLOPT_RESUME_FROM, downloaded_bytes);
	curl_easy_setopt(curl_handle, CURLOPT_BUFFERSIZE, 524288);
	struct curl_slist *headerchunk = NULL;
	headerchunk = curl_slist_append(headerchunk, "Accept: */*");
	headerchunk = curl_slist_append(headerchunk, "Content-Type: application/json");
	headerchunk = curl_slist_append(headerchunk, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.36");
	headerchunk = curl_slist_append(headerchunk, "Content-Length: 0");
	curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headerchunk);
	curl_easy_perform(curl_handle);
}

static int appListThread(unsigned int args, void *arg) {
	curl_handle = curl_easy_init();
	downloader_pass = 1;
	downloaded_bytes = 0;

	SceIoStat stat;
	sceIoGetstat("ux0:data/VitaDB/apps.json", &stat);
	total_bytes = stat.st_size;

	startDownload("https://vitadb.rinnegatamante.it/list_hbs_json.php");

	if (downloaded_bytes > 12 * 1024) {
		fh = fopen("ux0:data/VitaDB/apps.json", "wb");
		fwrite(generic_mem_buffer, 1, downloaded_bytes, fh);
		fclose(fh);
	}
	downloaded_bytes = total_bytes;
	curl_easy_cleanup(curl_handle);
	sceKernelExitDeleteThread(0);
	return 0;
}

static int downloadThread(unsigned int args, void *arg) {
	curl_handle = curl_easy_init();
	//printf("downloading %s\n", generic_url);
	char *url = (char *)generic_url;
	char *space = strstr(url, " ");
	char *s = url;
	char final_url[512] = "";
	fh = NULL;
	while (space) {
		space[0] = 0;
		sprintf(final_url, "%s%s%%20", final_url, s);
		space[0] = ' ';
		s = space + 1;
		space = strstr(s, " ");
	}
	sprintf(final_url, "%s%s", final_url, s);
	//printf("starting download of %s\n", final_url);
	downloader_pass = 1;
	downloaded_bytes = 0;
	total_bytes = 180; /* 20 KB */
	startDownload(final_url);
	if (downloaded_bytes > 180 && total_bytes <= MEM_BUFFER_SIZE) {
		fh = fopen(TEMP_DOWNLOAD_NAME, "wb");
		fwrite(generic_mem_buffer, 1, downloaded_bytes, fh);
	}
	fclose(fh);
	downloaded_bytes = total_bytes;
	curl_easy_cleanup(curl_handle);
	return sceKernelExitDeleteThread(0);
}

char *extractValue(char *dst, char *src, char *val, char **new_ptr) {
	char label[32];
	sprintf(label, "\"%s\": \"", val);
	//printf("label: %s\n", label);
	char *ptr = strstr(src, label) + strlen(label);
	//printf("ptr is: %X\n", ptr);
	if (ptr == strlen(label))
		return nullptr;
	char *end2 = strstr(ptr, val[0] == 'l' ? "\"," : "\"");
	if (dst == nullptr) {
		dst = malloc(end2 - ptr + 1);
		*new_ptr = dst;
	}
	//printf("size: %d\n", end2 - ptr);
	memcpy(dst, ptr, end2 - ptr);
	dst[end2 - ptr] = 0;
	return end2 + 1;
}

char *unescape(char *src) {
	char *res = malloc(strlen(src) + 1);
	uint32_t i = 0;
	char *s = src;
	while (*s) {
		char c = *s;
		int incr = 1;
		if (c == '\\' && s[1]) {
			switch (s[1]) {
			case '\\':
				c = '\\';
				incr = 2;
				break;
			case 'n':
				c = '\n';
				incr = 2;
				break;
			case 't':
				c = '\t';
				incr = 2;
				break;
			case '\'':
				c = '\'';
				incr = 2;
				break;
			case '\"':
				c = '\"';
				incr = 2;
				break;
			default:
				break;
			}
		}
		res[i++] = c;
		s += incr;
	}
	res[i] = 0;
	free(src);
	return res;
}

bool update_detected = false;
AppSelection *AppendAppDatabase(const char *file) {
	AppSelection *res = nullptr;
	FILE *f = fopen(file, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		uint64_t len = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *buffer = (char*)malloc(len + 1);
		fread(buffer, 1, len, f);
		buffer[len] = 0;
		char *ptr = buffer;
		char *end, *end2;
		do {
			char name[128], version[64];
			//printf("extract\n");
			ptr = extractValue(name, ptr, "name", nullptr);
			if (!ptr)
				break;
			AppSelection *node = (AppSelection*)malloc(sizeof(AppSelection));
			node->desc = nullptr;
			ptr = extractValue(node->icon, ptr, "icon", nullptr);
			ptr = extractValue(version, ptr, "version", nullptr);
			ptr = extractValue(node->author, ptr, "author", nullptr);
			ptr = extractValue(node->type, ptr, "type", nullptr);
			ptr = extractValue(node->id, ptr, "id", nullptr);
			if (!strncmp(node->id, "877", 3)) { // VitaDB Downloader, check if newer than running version
				if (strncmp(&version[2], VERSION, 3)) {
					res = node;
					update_detected = true;
				}
			}
			ptr = extractValue(node->date, ptr, "date", nullptr);
			ptr = extractValue(node->screenshots, ptr, "screenshots", nullptr);
			ptr = extractValue(node->desc, ptr, "long_description", &node->desc);
			node->desc = unescape(node->desc);
			ptr = extractValue(node->downloads, ptr, "downloads", nullptr);
			ptr = extractValue(node->size, ptr, "size", nullptr);
			ptr = extractValue(node->data_size, ptr, "data_size", nullptr);
			ptr = extractValue(node->data_link, ptr, "data", nullptr);
			sprintf(node->name, "%s %s", name, version);
			node->next = apps;
			apps = node;
		} while (ptr);
		fclose(f);
		free(buffer);
	}
	return res;
}

static char fname[512], ext_fname[512], read_buffer[8192];

void extract_file(char *file, char *dir) {
	unz_global_info global_info;
	unz_file_info file_info;
	unzFile zipfile = unzOpen(file);
	unzGetGlobalInfo(zipfile, &global_info);
	unzGoToFirstFile(zipfile);
	uint64_t total_extracted_bytes = 0;
	uint64_t curr_extracted_bytes = 0;
	uint64_t curr_file_bytes = 0;
	int num_files = global_info.number_entry;
	for (int zip_idx = 0; zip_idx < num_files; ++zip_idx) {
		unzGetCurrentFileInfo(zipfile, &file_info, fname, 512, NULL, 0, NULL, 0);
		total_extracted_bytes += file_info.uncompressed_size;
		if ((zip_idx + 1) < num_files) unzGoToNextFile(zipfile);
	}
	unzGoToFirstFile(zipfile);
	for (int zip_idx = 0; zip_idx < num_files; ++zip_idx) {
		unzGetCurrentFileInfo(zipfile, &file_info, fname, 512, NULL, 0, NULL, 0);
		sprintf(ext_fname, "%s%s", dir, fname); 
		const size_t filename_length = strlen(ext_fname);
		if (ext_fname[filename_length - 1] != '/') {
			curr_file_bytes = 0;
			unzOpenCurrentFile(zipfile);
			recursive_mkdir(ext_fname);
			FILE *f = fopen(ext_fname, "wb");
			while (curr_file_bytes < file_info.uncompressed_size) {
				int rbytes = unzReadCurrentFile(zipfile, read_buffer, 8192);
				if (rbytes > 0) {
					fwrite(read_buffer, 1, rbytes, f);
					curr_extracted_bytes += rbytes;
					curr_file_bytes += rbytes;
				}
				DrawExtractorDialog(zip_idx + 1, curr_file_bytes, curr_extracted_bytes, file_info.uncompressed_size, total_extracted_bytes, fname, num_files);
			}
			fclose(f);
			unzCloseCurrentFile(zipfile);
		}
		if ((zip_idx + 1) < num_files) unzGoToNextFile(zipfile);
	}
	unzClose(zipfile);
	ImGui::GetIO().MouseDrawCursor = false;
}

void download_file(char *url, char *text) {
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	int res = 0;
	SceUID thd = sceKernelCreateThread("Generic Downloader", &downloadThread, 0x10000100, 0x100000, 0, 0, NULL);
	sprintf(generic_url, url);
	sceKernelStartThread(thd, 0, NULL);
	do {
		DrawDownloaderDialog(downloader_pass, downloaded_bytes, total_bytes, text, 1, true);
		res = sceKernelGetThreadInfo(thd, &info);
	} while (info.status <= SCE_THREAD_DORMANT && res >= 0);
}

static int preview_width, preview_height, preview_x, preview_y;
GLuint preview_icon = 0, preview_shot = 0, previous_frame = 0;
bool need_icon = false;
int show_screenshots = 0; // 0 = off, 1 = download, 2 = show
void LoadPreview(AppSelection *game) {
	if (old_hovered == game)
		return;
	old_hovered = game;
	
	char banner_path[256];
	sprintf(banner_path, "ux0:data/VitaDB/icons/%s", game->icon);
	uint8_t *icon_data = stbi_load(banner_path, &preview_width, &preview_height, NULL, 4);
	if (icon_data) {
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
	} else {
		need_icon = true;
	}
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

void swap_apps(AppSelection *a, AppSelection *b) {
	AppSelection tmp;
	
	// Swapping everything except next leaf pointer
	sceClibMemcpy(&tmp, a, sizeof(AppSelection) - 4);
	sceClibMemcpy(a, b, sizeof(AppSelection) - 4);
	sceClibMemcpy(b, &tmp, sizeof(AppSelection) - 4);
}

const char *sort_modes_str[] = {
	"Most Recent",
	"Oldest",
	"Most Downloaded",
	"Least Downloaded",
	"Alphabetical (A-Z)",
	"Alphabetical (Z-A)",
};
const char *filter_modes[] = {
	"All Categories",
	"Original Games",
	"Game Ports",
	"Utilities",
	"Emulators",
};
int sort_idx = 0;
int old_sort_idx = -1;

void sort_applist(AppSelection *start) { 
	// Checking for empty list
	if (start == NULL) 
		return; 
	
	int swapped; 
	AppSelection *ptr1; 
	AppSelection *lptr = NULL; 
  
	do { 
		swapped = 0; 
		ptr1 = start; 
		
		int64_t d1, d2;
		char * dummy;
		while (ptr1->next != lptr && ptr1->next) {
			switch (sort_idx) {
			case 0: // Most Recent
				if (strcasecmp(ptr1->date, ptr1->next->date) < 0) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 1: // Oldest
				if (strcasecmp(ptr1->date, ptr1->next->date) > 0) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 2: // Most Downloaded
				d1 = strtoll(ptr1->downloads, &dummy, 10);
				d2 = strtoll(ptr1->next->downloads, &dummy, 10);
				if (d1 < d2) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 3: // Least Downloaded
				d1 = strtoll(ptr1->downloads, &dummy, 10);
				d2 = strtoll(ptr1->next->downloads, &dummy, 10);
				if (d1 > d2) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 4: // A-Z
				if (strcasecmp(ptr1->name, ptr1->next->name) > 0) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 5: // Z-A
				if (strcasecmp(ptr1->name, ptr1->next->name) < 0) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			default:
				break;
			}
			ptr1 = ptr1->next; 
		} 
		lptr = ptr1; 
	} while (swapped); 
}

bool filterApps(AppSelection *p) {
	if (filter_idx) {
		int filter_cat = filter_idx > 2 ? (filter_idx + 1) : filter_idx;
		if (p->type[0] - '0' != filter_cat)
			return true;
	}
	return false;
}

void scePromoterUtilInit() {
	uint32_t ptr[0x100] = { 0 };
	ptr[0] = 0;
	ptr[1] = (uint32_t)&ptr[0];
	uint32_t scepaf_argp[] = { 0x400000, 0xEA60, 0x40000, 0, 0 };
	sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(scepaf_argp), scepaf_argp, (SceSysmoduleOpt *)ptr);
	sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
	scePromoterUtilityInit();
}

void scePromoterUtilTerm() {
	scePromoterUtilityExit();
	sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
	SceSysmoduleOpt opt;
	sceClibMemset(&opt.flags, 0, sizeof(opt));
	sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, &opt);
}

static int musicThread(unsigned int args, void *arg) {
	// Starting background music
	SoLoud::Soloud audio_engine;
	SoLoud::Wav bg_mus;
	audio_engine.init();
	if (!bg_mus.load("ux0:/data/VitaDB/bg.ogg")) {
		bg_mus.setLooping(true);
		audio_engine.playBackground(bg_mus);
	} else {
		return sceKernelExitDeleteThread(0);
	}
	for (;;) {
		sceKernelDelayThread(500 * 1000 * 1000);
	}
}

int main(int argc, char *argv[]) {
	SceIoStat st1, st2;
	// Checking for libshacccg.suprx existence
	if (!(sceIoGetstat("ur0:/data/libshacccg.suprx", &st1) >= 0 || sceIoGetstat("ur0:/data/external/libshacccg.suprx", &st2) >= 0))
		early_fatal_error("Error: Runtime shader compiler (libshacccg.suprx) is not installed.");
	
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	sceIoMkdir("ux0:data/VitaDB", 0777);
	
	// Initializing sceAppUtil
	SceAppUtilInitParam appUtilParam;
	SceAppUtilBootParam appUtilBootParam;
	memset(&appUtilParam, 0, sizeof(SceAppUtilInitParam));
	memset(&appUtilBootParam, 0, sizeof(SceAppUtilBootParam));
	sceAppUtilInit(&appUtilParam, &appUtilBootParam);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &console_language);
	
	// Initializing sceCommonDialog
	SceCommonDialogConfigParam cmnDlgCfgParam;
	sceCommonDialogConfigParamInit(&cmnDlgCfgParam);
	cmnDlgCfgParam.language = (SceSystemParamLang)console_language;
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cmnDlgCfgParam.enterButtonAssign);
	sceCommonDialogSetConfigParam(&cmnDlgCfgParam);
	
	// Initializing sceNet
	generic_mem_buffer = (uint8_t*)malloc(MEM_BUFFER_SIZE);
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	int ret = sceNetShowNetstat();
	SceNetInitParam initparam;
	if (ret == SCE_NET_ERROR_ENOTINIT) {
		initparam.memory = malloc(141 * 1024);
		initparam.size = 141 * 1024;
		initparam.flags = 0;
		sceNetInit(&initparam);
	}
	
	// Initializing vitaGL
	AppSelection *hovered = nullptr;
	AppSelection *to_download = nullptr;
	vglInitExtended(0, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_NONE);

	ImGui::CreateContext();
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	int res = 0;
	ImGui_ImplVitaGL_Init();
	ImGui::GetIO().Fonts->AddFontFromFileTTF("app0:/Roboto.ttf", 16.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::GetIO().MouseDrawCursor = false;
	ImGui_ImplVitaGL_TouchUsage(false);
	ImGui_ImplVitaGL_GamepadUsage(true);
	ImGui_ImplVitaGL_MouseStickUsage(false);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 0);
	
	// Start background audio playback
	SceUID thd = sceKernelCreateThread("Audio Playback", &musicThread, 0x10000100, 0x100000, 0, 0, NULL);
	sceKernelStartThread(thd, 0, NULL);
	
	// Downloading apps list
	thd = sceKernelCreateThread("Apps List Downloader", &appListThread, 0x10000100, 0x100000, 0, 0, NULL);
	sceKernelStartThread(thd, 0, NULL);
	do {
		DrawDownloaderDialog(downloader_pass, downloaded_bytes, total_bytes, "Downloading apps list", 1, true);
		res = sceKernelGetThreadInfo(thd, &info);
	} while (info.status <= SCE_THREAD_DORMANT && res >= 0);
	hovered = AppendAppDatabase("ux0:data/VitaDB/apps.json");
	
	// Downloading icons
	SceIoStat stat;
	bool needs_icons_pack = false;
	if (sceIoGetstat("ux0:data/VitaDB/icons", &stat) < 0) {
		needs_icons_pack = true;
	} else {
		time_t cur_time, last_time;
		cur_time = time(NULL);
		FILE *f = fopen("ux0:data/VitaDB/last_boot.txt", "r");
		if (f) {
			fscanf(f, "%ld", &last_time);
			fclose(f);
			//sceClibPrintf("cur time is %ld and last time is %ld\n", cur_time, last_time);
			if (cur_time - last_time > 2592000) {
				init_interactive_msg_dialog("A long time passed since your last visit. Do you want to download all apps icons in one go?");
				while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
					vglSwapBuffers(GL_TRUE);
				}
				// Workaround to prevent message dialog "burn in" on background
				for (int i = 0; i < 10; i++) {
					glClear(GL_COLOR_BUFFER_BIT);
					vglSwapBuffers(GL_FALSE);
				}
				SceMsgDialogResult msg_res;
				memset(&msg_res, 0, sizeof(SceMsgDialogResult));
				sceMsgDialogGetResult(&msg_res);
				sceMsgDialogTerm();
				if (msg_res.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
					needs_icons_pack = true;
				}
			}
		}
		f = fopen("ux0:data/VitaDB/last_boot.txt", "w");
		fprintf(f, "%ld", cur_time);
		fclose(f);
	}
	if (needs_icons_pack) {
		download_file("https://vitadb.rinnegatamante.it/icons_zip.php", "Downloading apps icons");
		sceIoMkdir("ux0:data/VitaDB/icons", 0777);
		extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/icons/");
		sceIoRemove(TEMP_DOWNLOAD_NAME);
	}
	
	//printf("start\n");
	char ver_str[64];
	bool calculate_ver_len = true;
	bool go_to_top = false;
	bool fast_increment = false;
	bool fast_decrement = false;
	bool is_app_hovered;
	float ver_len = 0.0f;
	uint32_t oldpad;
	int filtered_entries;
	AppSelection *decrement_stack[4096];
	AppSelection *decremented_app = nullptr;
	int decrement_stack_idx = 0;
	while (!update_detected) {
		if (old_sort_idx != sort_idx) {
			old_sort_idx = sort_idx;
			sort_applist(apps);
		}
		
		ImGui_ImplVitaGL_NewFrame();
		
		if (ImGui::BeginMainMenuBar()) {
			char title[256];
			sprintf(title, "VitaDB Downloader - Currently listing %d results", filtered_entries);
			ImGui::Text(title);
			if (calculate_ver_len) {
				calculate_ver_len = false;
				sprintf(ver_str, "v.%s", VERSION);
				ImVec2 ver_sizes = ImGui::CalcTextSize(ver_str);
				ver_len = ver_sizes.x;
			}
			ImGui::SetCursorPosX(950 - ver_len);
			ImGui::Text(ver_str); 
			ImGui::EndMainMenuBar();
		}
		
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
		if (go_to_top) {
			ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
			ImGui::SetScrollHere();
			go_to_top = false;
		}
		ImGui::PopStyleVar();
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Category: ");
		ImGui::SameLine();
		ImGui::PushItemWidth(-1.0f);
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
		ImGui::PopItemWidth();
		ImGui::Separator();
		
		AppSelection *g = apps;
		filtered_entries = 0;
		int increment_idx = 0;
		is_app_hovered = false;
		while (g) {
			if (filterApps(g)) {
				g = g->next;
				continue;
			}
			if ((strlen(app_name_filter) == 0) || (strlen(app_name_filter) > 0 && (strcasestr(g->name, app_name_filter) || strcasestr(g->author, app_name_filter)))) {
				if (ImGui::Button(g->name, ImVec2(-1.0f, 0.0f))) {
					to_download = g;
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
					if (increment_idx == 21) {
						ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
						ImGui::SetScrollHere();
						increment_idx = 0;
					}
				} else if (fast_decrement) {
					if (!decremented_app)
						decrement_stack[decrement_stack_idx++] = g;
					else if (decremented_app == g) {
						ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
						ImGui::SetScrollHere();
						fast_decrement = false;
					}	
				}
				filtered_entries++;
			}
			g = g->next;
		}
		if (decrement_stack_idx == filtered_entries || !is_app_hovered)
			fast_decrement = false;
		fast_increment = false;
		ImGui::End();

		ImGui::SetNextWindowPos(ImVec2(553, 21), ImGuiSetCond_Always);
		ImGui::SetNextWindowSize(ImVec2(407, 523), ImGuiSetCond_Always);
		ImGui::Begin("Info Window", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
		if (hovered) {
			LoadPreview(hovered);
			ImGui::SetCursorPos(ImVec2(preview_x + PREVIEW_PADDING, preview_y + PREVIEW_PADDING));
			ImGui::Image((void*)preview_icon, ImVec2(preview_width, preview_height));
			ImGui::TextColored(ImVec4(1.0f, 1.0f, 0, 1.0f), "Description:");
			ImGui::TextWrapped(hovered->desc);
			ImGui::SetCursorPosY(6);
			ImGui::SetCursorPosX(140);
			ImGui::TextColored(ImVec4(1.0f, 1.0f, 0, 1.0f), "Last Update:");
			ImGui::SetCursorPosY(22);
			ImGui::SetCursorPosX(140);
			ImGui::Text(hovered->date);
			ImGui::SetCursorPosY(6);
			ImGui::SetCursorPosX(330);
			ImGui::TextColored(ImVec4(1.0f, 1.0f, 0, 1.0f), "Downloads:");
			ImGui::SetCursorPosY(22);
			ImGui::SetCursorPosX(330);
			ImGui::Text(hovered->downloads);
			ImGui::SetCursorPosY(38);
			ImGui::SetCursorPosX(140);
			ImGui::TextColored(ImVec4(1.0f, 1.0f, 0, 1.0f), "Category:");
			ImGui::SetCursorPosY(54);
			ImGui::SetCursorPosX(140);
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
			ImGui::SetCursorPosX(140);
			ImGui::TextColored(ImVec4(1.0f, 1.0f, 0, 1.0f), "Author:");
			ImGui::SetCursorPosY(86);
			ImGui::SetCursorPosX(140);
			ImGui::Text(hovered->author);
			ImGui::SetCursorPosY(100);
			ImGui::SetCursorPosX(140);
			ImGui::TextColored(ImVec4(1.0f, 1.0f, 0, 1.0f), "Size:");
			ImGui::SetCursorPosY(116);
			ImGui::SetCursorPosX(140);
			char sizes[64];
			char *dummy;
			int64_t sz;
			if (strlen(hovered->data_link) > 5) {
				sz = strtoll(hovered->size, &dummy, 10);
				int64_t sz2 = strtoll(hovered->data_size, &dummy, 10);
				sprintf(sizes, "VPK: %.2f MBs, Data: %.2f MBs", (float)sz / (float)(1024 * 1024), (float)sz2 / (float)(1024 * 1024));
			} else {
				sz = strtoll(hovered->size, &dummy, 10);
				sprintf(sizes, "VPK: %.2f MBs", (float)sz / (float)(1024 * 1024));
			}
			ImGui::Text(sizes);
		}
		ImGui::SetCursorPosY(470);
		ImGui::Text("Current sorting mode: %s", sort_modes_str[sort_idx]);
		ImGui::SetCursorPosY(486);
		ImGui::TextColored(ImVec4(1.0f, 1.0f, 0, 1.0f), "Press L/R to change sorting mode");
		ImGui::SetCursorPosY(502);
		if (hovered && strlen(hovered->screenshots) > 5) {
			ImGui::TextColored(ImVec4(1.0f, 1.0f, 0, 1.0f), "Press Start to view screenshots");
		}
		ImGui::End();
		
		if (show_screenshots == 2) {
			LoadScreenshot();
			ImGui::SetNextWindowPos(ImVec2(80, 55), ImGuiSetCond_Always);
			ImGui::SetNextWindowSize(ImVec2(800, 472), ImGuiSetCond_Always);
			ImGui::Begin("Screenshots Viewer (Left/Right to change current screenshot)", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			ImGui::Image((void*)preview_shot, ImVec2(800 - 19, 453 - 19));
			ImGui::End();
		}
		
		glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
		ImGui::Render();
		ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
		vglSwapBuffers(GL_FALSE);
		
		// Extra controls handling
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_LTRIGGER && !(oldpad & SCE_CTRL_LTRIGGER) && !show_screenshots) {
			sort_idx -= 1;
			if (sort_idx < 0)
				sort_idx = (sizeof(sort_modes_str) / sizeof(sort_modes_str[0])) - 1;
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_RTRIGGER && !(oldpad & SCE_CTRL_RTRIGGER) && !show_screenshots) {
			sort_idx = (sort_idx + 1) % (sizeof(sort_modes_str) / sizeof(sort_modes_str[0]));
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_START && !(oldpad & SCE_CTRL_START) && hovered && strlen(hovered->screenshots) > 5) {
			show_screenshots = show_screenshots ? 0 : 1;
		} else if (pad.buttons & SCE_CTRL_LEFT && !(oldpad & SCE_CTRL_LEFT)) {
			if (show_screenshots)
				cur_ss_idx--;
			else {
				fast_decrement = true;
				decrement_stack_idx = 0;
				decremented_app = nullptr;
			}
		} else if (pad.buttons & SCE_CTRL_RIGHT && !(oldpad & SCE_CTRL_RIGHT)) {
			if (show_screenshots)
				cur_ss_idx++;
			else
				fast_increment = true;
		} else if (pad.buttons & SCE_CTRL_CIRCLE && !show_screenshots) {
			go_to_top = true;
		}
		oldpad = pad.buttons;
		
		// Queued app download
		if (to_download) {
			if (strlen(to_download->data_link) > 5) {
				uint8_t *scr_data = vglMalloc(960 * 544 * 4);
				glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, scr_data);
				if (!previous_frame)
					glGenTextures(1, &previous_frame);
				glBindTexture(GL_TEXTURE_2D, previous_frame);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 960, 544, 0, GL_RGBA, GL_UNSIGNED_BYTE, scr_data);
				vglFree(scr_data);
				init_interactive_msg_dialog("This homebrew has also data files. Do you wish to install them as well?");
				while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
					vglSwapBuffers(GL_TRUE);
				}
				glMatrixMode(GL_PROJECTION);
				glLoadIdentity();
				glOrthof(0, 960, 544, 0, 0, 1);
				glMatrixMode(GL_MODELVIEW);
				glLoadIdentity();
				float vtx[4 * 2] = {
					  0, 544,
					960, 544,
					  0,   0,
					960,   0
				};
				float txcoord[4 * 2] = {
					  0,   0,
					  1,   0,
					  0,   1,
					  1,   1
				};
				// Workaround to prevent message dialog "burn in" on background
				for (int i = 0; i < 10; i++) {
					glEnableClientState(GL_VERTEX_ARRAY);
					glEnableClientState(GL_TEXTURE_COORD_ARRAY);
					glVertexPointer(2, GL_FLOAT, 0, vtx);
					glTexCoordPointer(2, GL_FLOAT, 0, txcoord);
					glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
					vglSwapBuffers(GL_FALSE);
				}
				SceMsgDialogResult msg_res;
				memset(&msg_res, 0, sizeof(SceMsgDialogResult));
				sceMsgDialogGetResult(&msg_res);
				sceMsgDialogTerm();
				if (msg_res.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
					download_file(to_download->data_link, "Downloading data files");
					extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/");
					sceIoRemove(TEMP_DOWNLOAD_NAME);
				}
			}
			sprintf(download_link, "https://vitadb.rinnegatamante.it/get_hb_url.php?id=%s", to_download->id);
			download_file(download_link, "Downloading vpk");
			if (!strncmp(to_download->id, "877", 3)) { // Updating VitaDB Downloader
				sceAppMgrUmount("app0:");
				extract_file(TEMP_DOWNLOAD_NAME, "ux0:app/VITADBDLD/");
				sceIoRemove(TEMP_DOWNLOAD_NAME);
				sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
			} else {
				sceIoMkdir("ux0:data/VitaDB/vpk", 0777);
				extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/vpk/");
				sceIoRemove(TEMP_DOWNLOAD_NAME);
				makeHeadBin("ux0:data/VitaDB/vpk");
				scePromoterUtilInit();
				scePromoterUtilityPromotePkg("ux0:data/VitaDB/vpk", 0);
				int state = 0;
				do {
					int ret = scePromoterUtilityGetState(&state);
					if (ret < 0)
						break;
					DrawTextDialog("Installing the app", true);
					vglSwapBuffers(GL_TRUE);
				} while (state);
				scePromoterUtilTerm();
				to_download = nullptr;
			}
		}
		
		// Queued icon download
		if (need_icon) {
			sprintf(download_link, "https://rinnegatamante.it/vitadb/icons/%s", old_hovered->icon);				
			download_file(download_link, "Downloading missing icon");
			sprintf(download_link, "ux0:data/VitaDB/icons/%s", old_hovered->icon);
			sceIoRename(TEMP_DOWNLOAD_NAME, download_link);
			old_hovered = nullptr;
			need_icon = false;
		}
		
		// Ime dialog active
		if (is_ime_active) {
			while (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
				vglSwapBuffers(GL_TRUE);
			}
			SceImeDialogResult res;
			sceClibMemset(&res, 0, sizeof(SceImeDialogResult));
			sceImeDialogGetResult(&res);
			if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
				getDialogTextResult(app_name_filter);
			}
			sceImeDialogTerm();
			is_ime_active = false;
		}
		
		// Queued screenshots download
		if (show_screenshots == 1) {
			sceIoRemove("ux0:data/VitaDB/ss0.png");
			sceIoRemove("ux0:data/VitaDB/ss1.png");
			sceIoRemove("ux0:data/VitaDB/ss2.png");
			sceIoRemove("ux0:data/VitaDB/ss3.png");
			old_ss_idx = -1;
			cur_ss_idx = 0;
			char *s = hovered->screenshots;
			int shot_idx = 0;
			for (;;) {
				char *end = strstr(s, ";");
				if (end) {
					end[0] = 0;
				}
				sprintf(download_link, "https://vitadb.rinnegatamante.it/%s", s);				
				download_file(download_link, "Downloading screenshot");
				sprintf(download_link, "ux0:data/VitaDB/ss%d.png", shot_idx++);
				sceIoRename(TEMP_DOWNLOAD_NAME, download_link);
				if (end) {
					end[0] = ';';
					s = end + 1;
				} else {
					break;
				}
			}
			show_screenshots = 2;
		}
	}
	
	// Installing update
	download_file("https://vitadb.rinnegatamante.it/get_hb_url.php?id=877", "Downloading update");
	sceAppMgrUmount("app0:");
	extract_file(TEMP_DOWNLOAD_NAME, "ux0:app/VITADBDLD/");
	sceIoRemove(TEMP_DOWNLOAD_NAME);
	sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
}
