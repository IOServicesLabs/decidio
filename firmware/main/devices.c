// Smart Life / Tuya devices on the same network, switched by rules. Local only: the board talks to the device's own
// TCP port 6668 and never to the Tuya cloud. Each device needs its id and its 16-character local key (from the
// account the device is paired with); the address and protocol version are learned from the device's own broadcasts.
//
// Protocol versions 3.1, 3.3, 3.4 and 3.5 (the frame layouts and key steps are the ones in tinytuya, the reference
// client; tools/fake_tuya_device.py is a device to test against):
//   3.1  55AA frame + CRC32; a command is "3.1" + part of an MD5 + base64(AES-ECB(json)); a status request is plain
//   3.3  55AA frame + CRC32; AES-ECB(json), a command has "3.3" + 12 zero bytes in front
//   3.4  55AA frame + HMAC-SHA256; everything AES-ECB with a session key agreed in three messages
//   3.5  6699 frame, AES-GCM, the same three-message session key
// One connection per command. A device takes few connections at a time, and the phone app may hold one.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_rom_md5.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "rt.h"

#define PORT 6668
#define BUF 1400
#define MAX_FOUND 12
#define CMD_SET 1
#define CMD_STATUS 2

typedef struct {
    bool used;
    char name[20], id[24], key[17], ip[16];
    uint8_t ver, dp;            // ver: 31 | 33 | 34 | 35
    uint16_t mask;              // "follow" rules that are on now (bit = rule index)
    int8_t state;               // what was last sent: -1 nothing yet
    bool stale;                 // rules that held it on were replaced: off soon unless a new rule holds it
    uint32_t ok, fail;
    char err[72];
} tdev_t;

typedef struct {
    char id[24], ip[16], product[20];
    uint8_t ver;
    int64_t seen_us;
} found_t;

typedef struct {
    uint8_t slot, on;
} job_t;

typedef struct {
    uint32_t cmd, ret;          // ret: the device's return code (0 = fine)
    uint8_t *p;                 // payload after the return code, decrypted
    int n;
} frame_t;

// a device's answer starts with a 4-byte return code (0 or 1); what follows never starts with three zero bytes
static void strip_ret(frame_t *f)
{
    f->ret = 0;
    if (f->n >= 4 && f->p[0] == 0 && f->p[1] == 0 && f->p[2] == 0) {
        f->ret = f->p[3];
        f->p += 4;
        f->n -= 4;
    }
}

static const char *TAG = "dev";
static tdev_t *s_dev;
static found_t *s_found;
static SemaphoreHandle_t s_lock, s_io;
static QueueHandle_t s_q;
static int s_udp[2] = { -1, -1 };
static uint8_t *s_ubuf;            // 512 bytes for a device's broadcast, in external memory
static const uint8_t UDP_KEY[16] = { 0x6c, 0x1e, 0xc8, 0xe2, 0xbb, 0x9b, 0xb5, 0x9a, 0xb5, 0x0b, 0x0d, 0xaf, 0x64, 0x9b, 0x41, 0x0a };  // md5("yGAdlopoPVldABfn")

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void hmac(const uint8_t *key, const uint8_t *data, size_t n, uint8_t out[32])
{
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 16, data, n, out);
}

// AES-128-ECB with PKCS7 padding; `out` needs n + 16 bytes. pad false: n must be a multiple of 16.
static int ecb_encrypt(const uint8_t *key, const uint8_t *in, int n, uint8_t *out, bool pad)
{
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    mbedtls_aes_setkey_enc(&a, key, 128);
    const int full = pad ? (n / 16 + 1) * 16 : n;
    uint8_t blk[16];
    for (int at = 0; at < full; at += 16) {
        for (int i = 0; i < 16; i++) {
            blk[i] = at + i < n ? in[at + i] : (uint8_t)(full - n);
        }
        mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_ENCRYPT, blk, out + at);
    }
    mbedtls_aes_free(&a);
    return full;
}

// in place; returns the length without padding, or -1
static int ecb_decrypt(const uint8_t *key, uint8_t *buf, int n, bool pad)
{
    if (n <= 0 || n % 16) {
        return -1;
    }
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    mbedtls_aes_setkey_dec(&a, key, 128);
    for (int at = 0; at < n; at += 16) {
        mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_DECRYPT, buf + at, buf + at);
    }
    mbedtls_aes_free(&a);
    if (!pad) {
        return n;
    }
    const int k = buf[n - 1];
    return k >= 1 && k <= 16 ? n - k : -1;
}

static int gcm(bool enc, const uint8_t *key, const uint8_t *iv, const uint8_t *aad, int aad_n, const uint8_t *in, int n,
               uint8_t *out, uint8_t *tag)
{
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int r = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (r == 0) {
        r = enc ? mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, iv, 12, aad, aad_n, in, out, 16, tag)
                : mbedtls_gcm_auth_decrypt(&g, n, iv, 12, aad, aad_n, tag, 16, in, out);
    }
    mbedtls_gcm_free(&g);
    return r;
}

// ---- frames ----

// 55AA: prefix | seq | cmd | len | payload | crc32 or hmac | suffix
static int frame_55aa(uint8_t *out, uint32_t seq, uint32_t cmd, const uint8_t *p, int n, const uint8_t *hmac_key)
{
    be32(out, 0x000055AA);
    be32(out + 4, seq);
    be32(out + 8, cmd);
    be32(out + 12, n + (hmac_key ? 36 : 8));
    memmove(out + 16, p, n);
    int at = 16 + n;
    if (hmac_key) {
        hmac(hmac_key, out, at, out + at);
        at += 32;
    } else {
        be32(out + at, esp_rom_crc32_le(0, out, at));
        at += 4;
    }
    be32(out + at, 0x0000AA55);
    return at + 4;
}

// 6699: prefix | 0 0 | seq | cmd | len | iv(12) | ciphertext | tag(16) | suffix; the header after the prefix is authenticated
static int frame_6699(uint8_t *out, uint32_t seq, uint32_t cmd, const uint8_t *p, int n, const uint8_t *key)
{
    be32(out, 0x00006699);
    out[4] = out[5] = 0;
    be32(out + 6, seq);
    be32(out + 10, cmd);
    be32(out + 14, n + 28);
    esp_fill_random(out + 18, 12);
    if (gcm(true, key, out + 18, out + 4, 14, p, n, out + 30, out + 30 + n) != 0) {
        return -1;
    }
    be32(out + 46 + n, 0x00009966);
    return 50 + n;
}

static bool recv_all(int fd, uint8_t *b, int n)
{
    int got = 0;
    while (got < n) {
        const int r = recv(fd, b + got, n - got, 0);
        if (r <= 0) {
            return false;
        }
        got += r;
    }
    return true;
}

// One frame from the device, checked and decrypted. key: the frame key (3.4 / 3.5), NULL for 3.1 / 3.3.
// Returns 0, -1 = the connection gave nothing (in time), -2 = a frame that does not check out (wrong key).
static int read_frame(int fd, uint8_t *b, int ver, const uint8_t *key, frame_t *f)
{
    if (!recv_all(fd, b, 4)) {
        return -1;
    }
    const uint32_t prefix = rd32(b);
    if (prefix == 0x000055AA) {
        if (!recv_all(fd, b + 4, 12)) {
            return -1;
        }
        const int len = (int)rd32(b + 12), end = ver >= 34 ? 36 : 8;
        if (len < end || len > BUF - 16 || !recv_all(fd, b + 16, len)) {
            return -1;
        }
        f->cmd = rd32(b + 8);
        const int body = 16 + len - end;                   // header + payload
        if (ver >= 34) {
            uint8_t want[32];
            hmac(key, b, body, want);
            if (memcmp(want, b + body, 32)) {
                return -2;
            }
        } else if (esp_rom_crc32_le(0, b, body) != rd32(b + body)) {
            return -2;
        }
        f->p = b + 16;
        f->n = len - end;
        strip_ret(f);
        if (ver == 34 && f->n > 0) {
            f->n = ecb_decrypt(key, f->p, f->n, true);
            if (f->n < 0) {
                return -2;
            }
        }
        return 0;
    }
    if (prefix == 0x00006699) {
        if (!recv_all(fd, b + 4, 14)) {
            return -1;
        }
        const int len = (int)rd32(b + 14);
        if (len < 28 || len > BUF - 22 || !recv_all(fd, b + 18, len + 4)) {
            return -1;
        }
        f->cmd = rd32(b + 10);
        const int n = len - 28;
        if (!key || gcm(false, key, b + 18, b + 4, 14, b + 30, n, b + 30, b + 30 + n) != 0) {
            return -2;
        }
        f->p = b + 30;
        f->n = n;
        strip_ret(f);
        return 0;
    }
    return -1;
}

static int send_frame(int fd, uint8_t *out, int ver, uint32_t *seq, uint32_t cmd, const uint8_t *p, int n, const uint8_t *key)
{
    int len;
    if (ver >= 35) {
        len = frame_6699(out, (*seq)++, cmd, p, n, key);
    } else if (ver == 34) {
        uint8_t *enc = out + 16;                           // encrypt straight into the frame
        n = ecb_encrypt(key, p, n, enc, true);
        len = frame_55aa(out, (*seq)++, cmd, enc, n, key);
    } else {
        len = frame_55aa(out, (*seq)++, cmd, p, n, NULL);
    }
    return len > 0 && send(fd, out, len, 0) == len ? 0 : -1;
}

static int dial(const char *ip, char *err, size_t cap)
{
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(PORT) };
    if (!ip[0] || inet_pton(AF_INET, ip, &to.sin_addr) != 1) {
        snprintf(err, cap, "address not known yet (waiting to hear the device)");
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, cap, "no socket free");
        return -1;
    }
    const struct timeval tv = { .tv_sec = 3 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&to, sizeof(to));
    if (r != 0 && errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        struct timeval wait = { .tv_sec = 3 };
        int e = 0;
        socklen_t el = sizeof(e);
        r = select(fd + 1, NULL, &w, NULL, &wait) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) == 0 && e == 0 ? 0 : -1;
    }
    if (r != 0) {
        close(fd);
        snprintf(err, cap, "no answer at %s (off, or the address changed)", ip);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    return fd;
}

// Send one command and wait for the device's answer. what: CMD_SET (on) or CMD_STATUS (answer JSON copied to `dps`).
static bool exchange(const tdev_t *d, int what, bool on, char *dps, size_t dps_cap, char *err, size_t cap)
{
    uint8_t *out = heap_caps_malloc_prefer(BUF * 2 + 1024, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!out) {
        snprintf(err, cap, "no memory");
        return false;
    }
    uint8_t *in = out + BUF, *tmp = in + BUF;              // out: the frame sent; in: frames read; tmp: 1024 bytes of scratch
    bool good = false;
    const int fd = dial(d->ip, err, cap);
    if (fd < 0) {
        free(out);
        return false;
    }
    uint8_t key[16], real[16];
    memcpy(real, d->key, 16);
    memcpy(key, real, 16);
    uint32_t seq = 1;
    frame_t f;
    const int ver = d->ver;
    snprintf(err, cap, "no answer to the command");

    if (ver >= 34) {                                       // agree a session key
        uint8_t mine[16], theirs[16], x[16], mac[32];
        esp_fill_random(mine, 16);
        if (send_frame(fd, out, ver, &seq, 3, mine, 16, real) != 0) {
            goto done;
        }
        const int r = read_frame(fd, in, ver, real, &f);
        if (r != 0 || f.cmd != 4 || f.n < 48) {
            snprintf(err, cap, r == -2 ? "the device's answer does not check out: wrong local key?" : "no answer to the key exchange: wrong local key?");
            goto done;
        }
        memcpy(theirs, f.p, 16);
        hmac(real, mine, 16, mac);
        if (memcmp(mac, f.p + 16, 32)) {
            snprintf(err, cap, "wrong local key");
            goto done;
        }
        hmac(real, theirs, 16, mac);
        if (send_frame(fd, out, ver, &seq, 5, mac, 32, real) != 0) {
            goto done;
        }
        for (int i = 0; i < 16; i++) {
            x[i] = mine[i] ^ theirs[i];
        }
        if (ver >= 35) {
            uint8_t tag[16];
            gcm(true, real, mine, NULL, 0, x, 16, key, tag);
        } else {
            ecb_encrypt(real, x, 16, key, false);
        }
    }

    // the command's plain payload
    int n;
    uint32_t cmd;
    const uint32_t t = (uint32_t)(esp_timer_get_time() / 1000000) + 1700000000u;
    char *js = (char *)tmp + 64;
    if (what == CMD_STATUS) {
        cmd = ver >= 34 ? 0x10 : 0x0a;
        n = ver >= 34 ? snprintf(js, 400, "{}")
                      : snprintf(js, 400, "{\"gwId\":\"%s\",\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lu\"}", d->id, d->id, d->id, (unsigned long)t);
    } else {
        cmd = ver >= 34 ? 0x0d : 0x07;
        n = ver >= 34 ? snprintf(js, 400, "{\"protocol\":5,\"t\":%lu,\"data\":{\"dps\":{\"%d\":%s}}}", (unsigned long)t, d->dp, on ? "true" : "false")
                      : snprintf(js, 400, "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lu\",\"dps\":{\"%d\":%s}}", d->id, d->id, (unsigned long)t, d->dp,
                                 on ? "true" : "false");
    }
    uint8_t *p = (uint8_t *)js;                            // js is at tmp + 64; `in` is free until the answer is read
    if (ver >= 34) {
        if (what == CMD_SET) {                             // "3.x" + 12 zero bytes in front of a command
            p -= 15;
            memset(p, 0, 15);
            p[0] = '3';
            p[1] = '.';
            p[2] = ver == 35 ? '5' : '4';
            n += 15;
        }
    } else if (ver == 33) {
        const int en = ecb_encrypt(real, p, n, in + 15, true);
        if (what == CMD_SET) {
            memset(in, 0, 15);
            memcpy(in, "3.3", 3);
            p = in;
            n = en + 15;
        } else {
            p = in + 15;
            n = en;
        }
    } else if (what == CMD_SET) {                          // 3.1: "3.1" + 16 hex of an MD5 + base64 of the encrypted JSON
        uint8_t *enc = in + 800, *b = in;                  // b: "data=" + base64 + "||lpv=3.1||" + key
        const int en = ecb_encrypt(real, p, n, enc, true);
        size_t bl = 0;
        memcpy(b, "data=", 5);
        mbedtls_base64_encode(b + 5, 700, &bl, enc, en);
        memcpy(b + 5 + bl, "||lpv=3.1||", 11);
        memcpy(b + 16 + bl, real, 16);
        md5_context_t m;
        uint8_t dig[16];
        esp_rom_md5_init(&m);
        esp_rom_md5_update(&m, b, 32 + bl);
        esp_rom_md5_final(dig, &m);
        char hex[33];
        for (int i = 0; i < 16; i++) {
            snprintf(hex + i * 2, 3, "%02x", dig[i]);
        }
        p = tmp;
        memcpy(p, "3.1", 3);
        memcpy(p + 3, hex + 8, 16);
        memcpy(p + 19, b + 5, bl);
        n = 19 + (int)bl;
    }
    if (send_frame(fd, out, ver, &seq, cmd, p, n, key) != 0) {
        snprintf(err, cap, "could not send");
        goto done;
    }

    for (int tries = 0; tries < 4 && !good; tries++) {
        const int r = read_frame(fd, in, ver, key, &f);
        if (r != 0) {
            if (r == -2) {
                snprintf(err, cap, "the device's answer does not check out: wrong local key?");
            }
            break;
        }
        if (what == CMD_SET) {
            if (f.cmd == cmd && f.ret != 0) {
                snprintf(err, cap, "the device refused the command%s", ver <= 33 ? ": wrong local key?" : "");
                break;
            }
            good = f.cmd == cmd || f.cmd == 8;             // the acknowledgement, or the state report that follows it
            continue;
        }
        if (f.cmd != cmd && f.cmd != 8) {
            continue;
        }
        if (ver == 33) {
            if (f.n >= 15 && !memcmp(f.p, "3.3", 3)) {
                f.p += 15;
                f.n -= 15;
            }
            f.n = ecb_decrypt(real, f.p, f.n, true);
        }
        if (f.n <= 0) {
            snprintf(err, cap, ver == 33 ? "the answer could not be read: wrong local key?" : "empty answer");
            continue;
        }
        f.p[f.n] = 0;
        const char *brace = memchr(f.p, '{', f.n);         // after an optional "3.x" header
        if (!brace) {
            snprintf(err, cap, "device says: %.30s", (const char *)f.p);
            continue;
        }
        if (dps) {
            snprintf(dps, dps_cap, "%s", brace);
        }
        good = true;
    }
    if (good) {
        err[0] = 0;
    } else if (what == CMD_SET && ver <= 33 && !strcmp(err, "no answer to the command")) {
        snprintf(err, cap, "the device did not accept the command: wrong local key?");
    }
done:
    close(fd);
    free(out);
    return good;
}

// ---- the list ----

static void load(void)
{
    char *blob = rt_store_dup("devices", NULL);
    cJSON *arr = blob ? cJSON_Parse(blob) : NULL;
    free(blob);
    const cJSON *j;
    cJSON_ArrayForEach(j, arr) {
        const cJSON *slot = cJSON_GetObjectItemCaseSensitive(j, "slot");
        if (!cJSON_IsNumber(slot) || slot->valueint < 0 || slot->valueint >= RT_MAX_DEV) {
            continue;
        }
        tdev_t *d = &s_dev[slot->valueint];
        const char *K[] = { "name", "id", "key", "ip" };
        char *dst[] = { d->name, d->id, d->key, d->ip };
        const size_t cap[] = { sizeof(d->name), sizeof(d->id), sizeof(d->key), sizeof(d->ip) };
        for (int i = 0; i < 4; i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, K[i]);
            snprintf(dst[i], cap[i], "%s", cJSON_IsString(v) ? v->valuestring : "");
        }
        const cJSON *ver = cJSON_GetObjectItemCaseSensitive(j, "ver"), *dp = cJSON_GetObjectItemCaseSensitive(j, "dp");
        d->ver = cJSON_IsNumber(ver) ? (uint8_t)ver->valueint : 33;
        d->dp = cJSON_IsNumber(dp) ? (uint8_t)dp->valueint : 1;
        d->state = -1;
        d->used = d->name[0] && d->id[0];
    }
    cJSON_Delete(arr);
}

static esp_err_t save(void)                                // with the lock held
{
    char *b = malloc(RT_MAX_DEV * 170 + 8);
    if (!b) {
        return ESP_ERR_NO_MEM;
    }
    int n = sprintf(b, "[");
    for (int i = 0; i < RT_MAX_DEV; i++) {
        const tdev_t *d = &s_dev[i];
        if (d->used) {
            n += sprintf(b + n, "%s{\"slot\":%d,\"name\":\"%s\",\"id\":\"%s\",\"key\":\"%s\",\"ip\":\"%s\",\"ver\":%d,\"dp\":%d}", n > 1 ? "," : "",
                         i, d->name, d->id, d->key, d->ip, d->ver, d->dp);
        }
    }
    sprintf(b + n, "]");
    const esp_err_t e = rt_store_set("devices", n > 1 ? b : NULL);
    free(b);
    return e;
}

void rt_dev_load(void)
{
    if (s_dev) {
        return;
    }
    s_dev = heap_caps_calloc_prefer(RT_MAX_DEV, sizeof(tdev_t), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    s_found = heap_caps_calloc_prefer(MAX_FOUND, sizeof(found_t), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    s_lock = xSemaphoreCreateMutex();
    s_io = xSemaphoreCreateMutex();
    s_q = xQueueCreate(12, sizeof(job_t));
    s_ubuf = heap_caps_malloc_prefer(512, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (!s_dev || !s_found || !s_lock || !s_io || !s_q || !s_ubuf) {
        ESP_LOGE(TAG, "no memory for the device list");
        s_dev = NULL;
        return;
    }
    load();
}

int rt_dev_index(const char *name)
{
    for (int i = 0; s_dev && name && i < RT_MAX_DEV; i++) {
        if (s_dev[i].used && !strcmp(s_dev[i].name, name)) {
            return i;
        }
    }
    return -1;
}

const char *rt_dev_name(int slot)
{
    return s_dev && slot >= 0 && slot < RT_MAX_DEV && s_dev[slot].used ? s_dev[slot].name : "";
}

static bool plain(const char *s, size_t max)               // short, and safe inside a JSON string
{
    return s && strlen(s) <= max && !strpbrk(s, "\"\\") && strspn(s, " ") != strlen(s);
}

// {"slot":0-7 (left out: the first free one),"name","id","key" (left out: unchanged),"ip","ver":"3.5","dp":1} | {"slot":n,"delete":true}
esp_err_t rt_dev_set(const cJSON *j, char *why, size_t cap)
{
    if (!s_dev) {
        snprintf(why, cap, "the device list is not available");
        return ESP_ERR_INVALID_STATE;
    }
    const cJSON *slot = cJSON_GetObjectItemCaseSensitive(j, "slot");
    int at = cJSON_IsNumber(slot) ? slot->valueint : -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "delete"))) {
        if (at >= 0 && at < RT_MAX_DEV && rt_actions_uses_dev(at)) {
            snprintf(why, cap, "a rule switches this device: take it out of the rule first");
        } else if (at >= 0 && at < RT_MAX_DEV) {
            memset(&s_dev[at], 0, sizeof(tdev_t));
            err = save();
        } else {
            snprintf(why, cap, "slot missing");
        }
        xSemaphoreGive(s_lock);
        return err;
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(j, "name"), *id = cJSON_GetObjectItemCaseSensitive(j, "id");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(j, "key"), *ip = cJSON_GetObjectItemCaseSensitive(j, "ip");
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(j, "ver"), *dp = cJSON_GetObjectItemCaseSensitive(j, "dp");
    for (int i = 0; at < 0 && i < RT_MAX_DEV; i++) {
        if (!s_dev[i].used) {
            at = i;
        }
    }
    tdev_t d = at >= 0 && at < RT_MAX_DEV ? s_dev[at] : (tdev_t){ 0 };
    const int v = cJSON_IsString(ver) ? (int)(atof(ver->valuestring) * 10 + 0.5) : cJSON_IsNumber(ver) ? ver->valueint : (d.used ? d.ver : 33);
    if (at < 0 || at >= RT_MAX_DEV) {
        snprintf(why, cap, "the board holds %d devices", RT_MAX_DEV);
    } else if (!cJSON_IsString(name) || !plain(name->valuestring, 19)) {
        snprintf(why, cap, "name: 1 to 19 characters");
    } else if (!cJSON_IsString(id) || !plain(id->valuestring, 23) || strlen(id->valuestring) < 8) {
        snprintf(why, cap, "id: the device id (20 or 22 characters)");
    } else if (cJSON_IsString(key) ? (strlen(key->valuestring) != 16 || !plain(key->valuestring, 16)) : !d.used) {
        snprintf(why, cap, "key: the local key is 16 characters");
    } else if (cJSON_IsString(ip) && ip->valuestring[0] && (strlen(ip->valuestring) > 15 || strspn(ip->valuestring, "0123456789.") != strlen(ip->valuestring))) {
        snprintf(why, cap, "ip: numbers and dots, or empty to find it");
    } else if (d.used && strcmp(d.name, name->valuestring) && rt_actions_uses_dev(at)) {
        snprintf(why, cap, "a rule names this device: take it out of the rule before renaming");
    } else if (v != 31 && v != 33 && v != 34 && v != 35) {
        snprintf(why, cap, "ver: 3.1, 3.3, 3.4 or 3.5");
    } else {
        for (int i = 0; i < RT_MAX_DEV; i++) {
            if (i != at && s_dev[i].used && !strcmp(s_dev[i].name, name->valuestring)) {
                snprintf(why, cap, "there is already a device called %s", name->valuestring);
                xSemaphoreGive(s_lock);
                return err;
            }
        }
        snprintf(d.name, sizeof(d.name), "%s", name->valuestring);
        snprintf(d.id, sizeof(d.id), "%s", id->valuestring);
        if (cJSON_IsString(key)) {
            snprintf(d.key, sizeof(d.key), "%s", key->valuestring);
        }
        if (cJSON_IsString(ip)) {
            snprintf(d.ip, sizeof(d.ip), "%s", ip->valuestring);
        }
        d.ver = (uint8_t)v;
        d.dp = cJSON_IsNumber(dp) && dp->valueint > 0 && dp->valueint < 256 ? (uint8_t)dp->valueint : (d.used ? d.dp : 1);
        if (!d.used) {
            d.state = -1;
        }
        d.used = true;
        d.err[0] = 0;
        s_dev[at] = d;
        err = save();
    }
    xSemaphoreGive(s_lock);
    return err;
}

#define ADD(...) n += snprintf(dst + n, n < (int)cap ? cap - n : 0, __VA_ARGS__)

// the list (never the keys), and the devices heard on the network
int rt_dev_json(char *dst, size_t cap)
{
    int n = 0;
    ADD("{\"max\":%d,\"devices\":[", RT_MAX_DEV);
    if (s_dev) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool any = false;
        for (int i = 0; i < RT_MAX_DEV; i++) {
            const tdev_t *d = &s_dev[i];
            if (!d->used) {
                continue;
            }
            ADD("%s{\"slot\":%d,\"name\":\"%s\",\"id\":\"%s\",\"ip\":\"%s\",\"ver\":\"%d.%d\",\"dp\":%d,\"state\":%s,\"sent\":%lu,\"failed\":%lu,"
                "\"error\":\"%s\"}", any ? "," : "", i, d->name, d->id, d->ip, d->ver / 10, d->ver % 10, d->dp,
                d->state < 0 ? "null" : d->state ? "true" : "false", (unsigned long)d->ok, (unsigned long)d->fail, d->err);
            any = true;
        }
        ADD("],\"found\":[");
        const int64_t now = esp_timer_get_time();
        any = false;
        for (int i = 0; i < MAX_FOUND; i++) {
            const found_t *f = &s_found[i];
            if (f->id[0] && now - f->seen_us < 120 * 1000000LL) {
                ADD("%s{\"id\":\"%s\",\"ip\":\"%s\",\"ver\":\"%d.%d\",\"product\":\"%s\"}", any ? "," : "", f->id, f->ip, f->ver / 10, f->ver % 10,
                    f->product);
                any = true;
            }
        }
        xSemaphoreGive(s_lock);
    } else {
        ADD("],\"found\":[");
    }
    ADD("]}");
    return n < (int)cap ? n : -1;
}

static bool run(int slot, int what, bool on, char *dps, size_t dps_cap, char *err, size_t cap)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const tdev_t d = s_dev[slot];
    xSemaphoreGive(s_lock);
    if (!d.used) {
        snprintf(err, cap, "no such device");
        return false;
    }
    if (!rt_wifi_online()) {
        snprintf(err, cap, "the board is not on Wi-Fi");
        return false;
    }
    xSemaphoreTake(s_io, portMAX_DELAY);
    const bool good = exchange(&d, what, on, dps, dps_cap, err, cap);
    xSemaphoreGive(s_io);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tdev_t *live = &s_dev[slot];
    if (live->used && !strcmp(live->id, d.id)) {
        if (good) {
            live->ok++;
            live->err[0] = 0;
            if (what == CMD_SET) {
                live->state = on;
            }
        } else {
            live->fail++;
            snprintf(live->err, sizeof(live->err), "%s", err);
        }
    }
    xSemaphoreGive(s_lock);
    return good;
}

// from the web page: "on" | "off" | "status" (the device's own answer goes to dps)
bool rt_dev_test(int slot, const char *what, rt_alert_result_t *out, char *dps, size_t dps_cap)
{
    memset(out, 0, sizeof(*out));
    if (dps_cap) {
        dps[0] = 0;
    }
    if (!s_dev || slot < 0 || slot >= RT_MAX_DEV) {
        snprintf(out->detail, sizeof(out->detail), "no such device");
        return false;
    }
    const int64_t t0 = esp_timer_get_time();
    const bool status = !strcmp(what, "status");
    out->ok = run(slot, status ? CMD_STATUS : CMD_SET, !strcmp(what, "on"), dps, dps_cap, out->detail, sizeof(out->detail));
    out->ms = (int)((esp_timer_get_time() - t0) / 1000);
    return out->ok;
}

// ---- rules ----

// A rule with a device turned on or off (from the decision task: never waits). "follow" rules share a device: it is on
// while any of them is on.
void rt_dev_rule(int slot, int rule, int mode, bool on)
{
    if (!s_dev || slot < 0 || slot >= RT_MAX_DEV || !s_dev[slot].used) {
        return;
    }
    tdev_t *d = &s_dev[slot];
    job_t j = { .slot = (uint8_t)slot };
    if (mode == RT_DEV_FOLLOW) {
        const uint16_t before = d->mask;
        d->mask = on ? (before | (1u << rule)) : (before & ~(1u << rule));
        if ((before != 0) == (d->mask != 0)) {
            return;
        }
        if (d->stale && on) {
            d->stale = false;                              // still on from the rules before: nothing to send
            return;
        }
        j.on = d->mask != 0;
    } else if (mode == RT_DEV_INVERSE) {
        j.on = !on;
    } else if (!on) {
        return;                                            // "switch on" / "switch off" act when the rule starts
    } else {
        j.on = mode == RT_DEV_ON;
    }
    xQueueSend(s_q, &j, 0);
}

void rt_dev_rules_reset(void)                              // the rules were replaced: nothing is holding a device on
{
    for (int i = 0; s_dev && i < RT_MAX_DEV; i++) {
        s_dev[i].stale = s_dev[i].stale || s_dev[i].mask != 0;
        s_dev[i].mask = 0;
    }
}

// a few seconds after the rules were replaced: a device the old rules held on, and no new rule holds, goes off
void rt_dev_settle(void)
{
    for (int i = 0; s_dev && i < RT_MAX_DEV; i++) {
        if (s_dev[i].used && s_dev[i].stale && s_dev[i].mask == 0) {
            const job_t j = { .slot = (uint8_t)i, .on = 0 };
            xQueueSend(s_q, &j, 0);
        }
        s_dev[i].stale = false;
    }
}

// ---- the devices' own broadcasts: who is there, at which address, speaking which version ----

static void heard(const char *js)
{
    cJSON *j = cJSON_Parse(js);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "gwId"), *ip = cJSON_GetObjectItemCaseSensitive(j, "ip");
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(j, "version"), *pk = cJSON_GetObjectItemCaseSensitive(j, "productKey");
    if (cJSON_IsString(id) && cJSON_IsString(ip) && plain(id->valuestring, 23) && plain(ip->valuestring, 15)) {
        const int v = cJSON_IsString(ver) ? (int)(atof(ver->valuestring) * 10 + 0.5) : 33;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int at = -1, oldest = 0;
        for (int i = 0; i < MAX_FOUND; i++) {
            if (!strcmp(s_found[i].id, id->valuestring)) {
                at = i;
            }
            if (s_found[i].seen_us < s_found[oldest].seen_us) {
                oldest = i;
            }
        }
        found_t *f = &s_found[at < 0 ? oldest : at];
        snprintf(f->id, sizeof(f->id), "%s", id->valuestring);
        snprintf(f->ip, sizeof(f->ip), "%s", ip->valuestring);
        snprintf(f->product, sizeof(f->product), "%s", cJSON_IsString(pk) && plain(pk->valuestring, 19) ? pk->valuestring : "");
        f->ver = (uint8_t)v;
        f->seen_us = esp_timer_get_time();
        for (int i = 0; i < RT_MAX_DEV; i++) {             // a device on the list that moved to another address
            if (s_dev[i].used && !strcmp(s_dev[i].id, f->id) && strcmp(s_dev[i].ip, f->ip)) {
                snprintf(s_dev[i].ip, sizeof(s_dev[i].ip), "%s", f->ip);
            }
        }
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(j);
}

static void listen_udp(void)
{
    static const uint16_t PORTS[2] = { 6666, 6667 };
    for (int k = 0; k < 2; k++) {
        if (s_udp[k] < 0 && rt_wifi_online()) {
            const int fd = socket(AF_INET, SOCK_DGRAM, 0);
            const struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORTS[k]), .sin_addr.s_addr = htonl(INADDR_ANY) };
            const int yes = 1;
            if (fd >= 0) {
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
                if (bind(fd, (const struct sockaddr *)&a, sizeof(a)) == 0) {
                    s_udp[k] = fd;
                } else {
                    close(fd);
                }
            }
        }
        if (s_udp[k] < 0) {
            continue;
        }
        uint8_t *b = s_ubuf;
        for (int burst = 0; burst < 4; burst++) {
            const int n = recv(s_udp[k], b, 511, MSG_DONTWAIT);
            if (n < 28) {
                break;
            }
            uint8_t *p = NULL;
            int pn = 0;
            const uint32_t prefix = rd32(b);
            if (prefix == 0x000055AA) {
                const int len = (int)rd32(b + 12);
                if (len < 12 || 16 + len > n) {
                    continue;
                }
                p = b + 20;                                // after the return code
                pn = len - 12;
                if (p[0] != '{') {
                    pn = ecb_decrypt(UDP_KEY, p, pn, true);
                }
            } else if (prefix == 0x00006699) {
                const int len = (int)rd32(b + 14);
                if (len < 28 || 22 + len > n) {
                    continue;
                }
                pn = len - 28;
                p = b + 30;
                if (gcm(false, UDP_KEY, b + 18, b + 4, 14, p, pn, p, p + pn) != 0) {
                    continue;
                }
                if (pn > 4 && p[0] != '{' && p[4] == '{') {
                    p += 4;
                    pn -= 4;
                }
            }
            if (p && pn > 2 && p[0] == '{') {
                p[pn] = 0;
                heard((const char *)p);
            }
        }
    }
}

// from the home task: send what the rules asked for, and listen for devices
void rt_dev_step(int wait_ms)
{
    if (!s_dev) {
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        return;
    }
    job_t j;
    if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(wait_ms)) == pdTRUE && j.slot < RT_MAX_DEV) {
        char err[72];
        for (int tries = 0; tries < 3; tries++) {
            if (run(j.slot, CMD_SET, j.on, NULL, 0, err, sizeof(err))) {
                break;
            }
            ESP_LOGW(TAG, "%s -> %s failed: %s", rt_dev_name(j.slot), j.on ? "on" : "off", err);
            vTaskDelay(pdMS_TO_TICKS(600));
            listen_udp();                                  // its address may just have been heard
        }
    }
    listen_udp();
}

void rt_dev_wake(void)                                     // the home task has other work: stop waiting for a job
{
    const job_t none = { .slot = 0xff };
    if (s_q) {
        xQueueSend(s_q, &none, 0);
    }
}
