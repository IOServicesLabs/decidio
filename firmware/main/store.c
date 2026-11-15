// Settings that outlive a reflash of the app or the model: Wi-Fi name and password, the page password, where alerts
// go, and rules edited on the web page. They live in their own NVS partition (`cfg`). Nothing here is encrypted: who
// can read the flash chip can read them.
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "rt.h"

#define PART "cfg"
#define NS "decidio"

static bool s_ok;

esp_err_t rt_store_init(void)
{
    esp_err_t err = nvs_flash_init_partition(PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(PART);
        err = nvs_flash_init_partition(PART);
    }
    s_ok = err == ESP_OK;
    return err;
}

// a copy the caller frees, with a 0 after it; NULL when the key is not set
char *rt_store_dup(const char *key, size_t *len)
{
    nvs_handle_t h;
    if (!s_ok || nvs_open_from_partition(PART, NS, NVS_READONLY, &h) != ESP_OK) {
        return NULL;
    }
    size_t n = 0;
    char *v = NULL;
    if (nvs_get_blob(h, key, NULL, &n) == ESP_OK && (v = malloc(n + 1)) != NULL) {
        if (nvs_get_blob(h, key, v, &n) == ESP_OK) {
            v[n] = 0;
            if (len) {
                *len = n;
            }
        } else {
            free(v);
            v = NULL;
        }
    }
    nvs_close(h);
    return v;
}

bool rt_store_get(const char *key, char *dst, size_t cap)
{
    char *v = rt_store_dup(key, NULL);
    if (!v) {
        if (cap) {
            dst[0] = 0;
        }
        return false;
    }
    strlcpy(dst, v, cap);
    free(v);
    return true;
}

// val NULL or "" removes the key
esp_err_t rt_store_set(const char *key, const char *val)
{
    nvs_handle_t h;
    if (!s_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_open_from_partition(PART, NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (val && val[0]) {
        err = nvs_set_blob(h, key, val, strlen(val));
    } else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t rt_store_wipe(void)
{
    s_ok = false;
    nvs_flash_deinit_partition(PART);
    esp_err_t err = nvs_flash_erase_partition(PART);
    return err == ESP_OK ? rt_store_init() : err;
}
