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
 
#include <iostream>
#include <string>
#include <locale>
#include <codecvt>
#include <stdio.h>
#include <string>
#include <vitasdk.h>
#include <vitaGL.h>
#include <imgui_vita.h>
#include <imgui_internal.h>
#include "utils.h"

#define SCR_WIDTH 960
#define SCR_HEIGHT 544

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

void early_warning(const char *msg) {
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
	sceMsgDialogTerm();
}

int init_interactive_msg_dialog(const char *fmt, ...) {
	va_list list;
	char msg[1024];

	va_start(list, fmt);
	vsnprintf(msg, sizeof(msg), fmt, list);
	va_end(list);
	
	SceMsgDialogUserMessageParam msg_param;
	sceClibMemset(&msg_param, 0, sizeof(msg_param));
	msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_YESNO;
	msg_param.msg = (SceChar8 *)msg;

	SceMsgDialogParam param;
	sceMsgDialogParamInit(&param);
	_sceCommonDialogSetMagicNumber(&param.commonParam);
	param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
	param.userMsgParam = &msg_param;

	return sceMsgDialogInit(&param);
}

int init_msg_dialog(const char *fmt, ...) {
	va_list list;
	char msg[1024];

	va_start(list, fmt);
	vsnprintf(msg, sizeof(msg), fmt, list);
	va_end(list);
  
	SceMsgDialogUserMessageParam msg_param;
	sceClibMemset(&msg_param, 0, sizeof(msg_param));
	msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
	msg_param.msg = (SceChar8 *)msg;

	SceMsgDialogParam param;
	sceMsgDialogParamInit(&param);
	_sceCommonDialogSetMagicNumber(&param.commonParam);
	param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
	param.userMsgParam = &msg_param;

	return sceMsgDialogInit(&param);
}

int init_warning(const char *fmt, ...) {
	va_list list;
	char msg[1024];

	va_start(list, fmt);
	vsnprintf(msg, sizeof(msg), fmt, list);
	va_end(list);
  
	SceMsgDialogUserMessageParam msg_param;
	sceClibMemset(&msg_param, 0, sizeof(msg_param));
	msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_NONE;
	msg_param.msg = (SceChar8 *)msg;

	SceMsgDialogParam param;
	sceMsgDialogParamInit(&param);
	_sceCommonDialogSetMagicNumber(&param.commonParam);
	param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
	param.userMsgParam = &msg_param;

	return sceMsgDialogInit(&param);
}

int init_progressbar_dialog(const char *fmt, ...) {
	vglInit(0);
	va_list list;
	char msg[1024];

	va_start(list, fmt);
	vsnprintf(msg, sizeof(msg), fmt, list);
	va_end(list);

	SceMsgDialogProgressBarParam msg_param;
	sceClibMemset(&msg_param, 0, sizeof(msg_param));
	msg_param.barType = SCE_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
	msg_param.msg = (const SceChar8*)msg;
	
	SceMsgDialogParam param;
	sceMsgDialogParamInit(&param);
	param.mode = SCE_MSG_DIALOG_MODE_PROGRESS_BAR;
	param.progBarParam = &msg_param;
	
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

void DrawExtractorDialog(int index, float file_extracted_bytes, float extracted_bytes, float file_total_bytes, float total_bytes, char *filename, int num_files) {
	ImGui_ImplVitaGL_NewFrame();
	
	char msg1[256], msg2[256];
	sprintf(msg1, "%s (%d / %d)", "Extracting archive...", index, num_files);
	sprintf(msg2, "%s (%.2f %s / %.2f %s)", filename, format_size(file_extracted_bytes), format_size_str(file_extracted_bytes), format_size(file_total_bytes), format_size_str(file_total_bytes));
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

void DrawDearchiverDialog(float file_extracted_bytes, float file_total_bytes, char *filename) {
	ImGui_ImplVitaGL_NewFrame();
	
	char msg1[256], msg2[256];
	sprintf(msg1, "%s", "Extracting fast archive...");
	sprintf(msg2, "%s (%.2f %s / %.2f %s)", filename, format_size(file_extracted_bytes), format_size_str(file_extracted_bytes), format_size(file_total_bytes), format_size_str(file_total_bytes));
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
	ImGui::ProgressBar(file_extracted_bytes / file_total_bytes, ImVec2(200, 0));
	
	ImGui::End();
	glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
	ImGui::Render();
	ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
	vglSwapBuffers(GL_FALSE);
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
}

void DrawDownloaderDialog(int index, float downloaded_bytes, float total_bytes, char *text, int passes, bool self_contained) {
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
	
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
		sprintf(msg, "%.2f %s / %.2f %s", format_size(downloaded_bytes), format_size_str(downloaded_bytes), format_size(total_bytes), format_size_str(total_bytes));
		pos = ImGui::CalcTextSize(msg);
		ImGui::SetCursorPos(ImVec2((400 - pos.x) / 2, 40));
		ImGui::Text(msg);
		ImGui::SetCursorPos(ImVec2(100, 60));
		ImGui::ProgressBar(downloaded_bytes / total_bytes, ImVec2(200, 0));
	} else {
		sprintf(msg, "%.2f %s", format_size(downloaded_bytes), format_size_str(downloaded_bytes));
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

void DrawTextDialog(char *text, bool self_contained, bool clear_screen) {
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
	
	if (clear_screen)
		glClear(GL_COLOR_BUFFER_BIT);
	
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
