/*
 * This file is part of VitaDB Downloader
 * Copyright 2026 Rinnegatamante
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
 */

#include <atomic>
#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vitasdk.h>
#include <vitaGL.h>
#include "reviews.h"
#include "utils.h"

#define REVIEWS_RESPONSE_LIMIT (512 * 1024)
#define REVIEWS_MAX_ENTRIES 128

static std::atomic<int> reviews_status(REVIEWS_IDLE);
static char reviews_id[8] = {};
static std::string reviews_error;
static std::vector<review> reviews;
static int reviews_count = 0;
static double reviews_average = 0.0;

static size_t reviews_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
	std::string *data = (std::string *)userdata;
	if (size && nmemb > SIZE_MAX / size) {
		return 0;
	}
	size_t bytes = size * nmemb;
	if (bytes > REVIEWS_RESPONSE_LIMIT - data->size()) {
		return 0;
	}
	data->append(ptr, bytes);
	return bytes;
}

static bool read_review_field(char *src, const char *key, std::string &value) {
	char *data = nullptr;
	char *end = get_value_from_json(nullptr, src, key, &data);
	if (!end) {
		return false;
	}
	value = data ? unescape(data) : "";
	free(data);
	return true;
}

static bool read_review_score(const std::string &value, double &score) {
	if (value.empty()) {
		return false;
	}
	char *end;
	score = strtod(value.c_str(), &end);
	return end != value.c_str() && !*end && score >= 0.0 && score <= 100.0;
}

static const char *review_object_end(const char *begin) {
	int depth = 0;
	bool in_string = false;
	bool escaped = false;
	for (const char *p = begin; *p; p++) {
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (*p == '\\') {
				escaped = true;
			} else if (*p == '"') {
				in_string = false;
			}
		} else if (*p == '"') {
			in_string = true;
		} else if (*p == '{') {
			depth++;
		} else if (*p == '}') {
			if (--depth == 0) {
				return p + 1;
			}
			if (depth < 0) {
				return nullptr;
			}
		}
	}
	return nullptr;
}

static bool parse_certified_reviews(const std::string &response,
	std::vector<review> &entries, int &count, double &average) {
	if (response.empty()) {
		return false;
	}
	std::string body = response;
	std::string count_text, average_text;
	if (!read_review_field(&body[0], "count", count_text) ||
		!read_review_field(&body[0], "average", average_text)) {
		return false;
	}
	char *end;
	long n = strtol(count_text.c_str(), &end, 10);
	if (count_text.empty() || *end || n < 0 || n > REVIEWS_MAX_ENTRIES) {
		return false;
	}
	count = (int)n;
	if (count == 0 && average_text == "null") {
		average = 0.0;
	} else if (!read_review_score(average_text, average)) {
		return false;
	}

	char *array = strstr(&body[0], "\"reviews\":");
	if (!array) {
		return false;
	}
	array += strlen("\"reviews\":");
	while (isspace((unsigned char)*array)) {
		array++;
	}
	if (*array++ != '[') {
		return false;
	}
	entries.reserve(count);
	for (int i = 0; i < count; i++) {
		while (isspace((unsigned char)*array)) {
			array++;
		}
		if (i) {
			if (*array++ != ',') {
				return false;
			}
			while (isspace((unsigned char)*array)) {
				array++;
			}
		}
		if (*array != '{') {
			return false;
		}
		const char *next = review_object_end(array);
		if (!next) {
			return false;
		}
		std::string object(array, next - array);
		review review;
		std::string score_text;
		if (!read_review_field(&object[0], "reviewer", review.reviewer) ||
			!read_review_field(&object[0], "comment", review.comment) ||
			!read_review_field(&object[0], "score", score_text) ||
			!read_review_score(score_text, review.score)) {
			return false;
		}
		entries.push_back(review);
		array = (char *)next;
	}
	while (isspace((unsigned char)*array)) {
		array++;
	}
	return *array == ']';
}

static int reviews_thread(unsigned int args, void *arg) {
	std::string response;
	std::vector<review> entries;
	int count = 0;
	double average = 0.0;
	CURL *curl = curl_easy_init();
	char post[48];
	snprintf(post, sizeof(post), "{\"hid\":\"%s\"}", reviews_id);
	struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_URL, "https://www.rinnegatamante.eu/vitadb/get_reviews.php");
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post));
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, reviews_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	CURLcode result = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	parse_certified_reviews(response, entries, count, average);
	reviews.swap(entries);
	reviews_count = count;
	reviews_average = average;
	reviews_status.store(REVIEWS_READY, std::memory_order_release);
	return sceKernelExitDeleteThread(0);
}

bool load_certified_reviews(const char *id) {
	if (reviews_status.load(std::memory_order_acquire) == REVIEWS_LOADING) {
		return false;
	}
	if (!id || !id[0] || strlen(id) >= sizeof(reviews_id)) {
		return false;
	}
	for (const char *c = id; *c; c++) {
		if (*c < '0' || *c > '9') {
			return false;
		}
	}
	strcpy(reviews_id, id);
	reviews.clear();
	reviews_error.clear();
	reviews_count = 0;
	reviews_average = 0.0;
	reviews_status.store(REVIEWS_LOADING, std::memory_order_release);
	SceUID thd = sceKernelCreateThread("Reviews Downloader", &reviews_thread, 0x10000100, 0x100000, 0, 0, NULL);
	sceKernelStartThread(thd, 0, NULL);
	return true;
}

uint8_t get_certified_reviews_status() {
	return (uint8_t)reviews_status.load(std::memory_order_acquire);
}

const char *get_certified_reviews_id() {
	return reviews_id;
}

const char *get_certified_reviews_error() {
	return reviews_error.c_str();
}

int get_certified_reviews_count() {
	return reviews_count;
}

double get_certified_reviews_average() {
	return reviews_average;
}

const std::vector<review> &get_certified_reviews() {
	return reviews;
}
