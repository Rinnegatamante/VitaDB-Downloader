#ifndef _REVIEWS_H
#define _REVIEWS_H

#include <string>
#include <vector>

enum {
	REVIEWS_IDLE,
	REVIEWS_LOADING,
	REVIEWS_READY,
	REVIEWS_ERROR
};

struct review {
	std::string reviewer;
	std::string comment;
	double score;
};

bool load_certified_reviews(const char *id);
uint8_t get_certified_reviews_status();
const char *get_certified_reviews_id();
const char *get_certified_reviews_error();
int get_certified_reviews_count();
double get_certified_reviews_average();
const std::vector<review> &get_certified_reviews();

#endif
