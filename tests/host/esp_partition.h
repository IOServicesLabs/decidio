// host stub: the `model` partition is a file (path in env DECIDIO_PART)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint32_t size; } esp_partition_t;
typedef int esp_partition_mmap_handle_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_MMAP_DATA 0
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_partition_mmap(const esp_partition_t *p, size_t off, size_t size, int memory, const void **out,
                             esp_partition_mmap_handle_t *h);
void esp_partition_munmap(esp_partition_mmap_handle_t h);
