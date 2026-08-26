#pragma once
#include <stdio.h>
/* Argument-checked, so a wrong format string still fails the check. */
#define ESP_LOGE(tag, ...) do { (void)(tag); printf(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); printf(__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); printf(__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, ...) do { (void)(tag); printf(__VA_ARGS__); } while (0)
#define ESP_LOGV(tag, ...) do { (void)(tag); printf(__VA_ARGS__); } while (0)
