/*
 * MedgeNet Client DLL
 *
 * Applies the runtime patches used by the MedgeNet launcher path.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../MedgeNetVersion.h"

#pragma comment(lib, "kernel32.lib")
int _fltused = 0;

void *__cdecl memcpy(void *dst, const void *src, size_t len)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (len--) *d++ = *s++;
    return dst;
}

void *__cdecl memset(void *dst, int value, size_t len)
{
    unsigned char *d = (unsigned char *)dst;
    while (len--) *d++ = (unsigned char)value;
    return dst;
}

/* ── Configuration ─────────────────────────────────────────────────── */

#define INI_SECTION "Server"
#define INI_FILE    "MedgeNetClient.ini"
#define LOG_FILE    "MedgeNetClient.log"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 18680
#define DEFAULT_HTTP_PORT 80

#define MAX_HOST_LEN 64
#define MAX_URL_LEN 1024

static char g_host[MAX_HOST_LEN];
static int  g_port;
static int  g_http_port;
static char g_ini_path[MAX_PATH];
static char g_log_path[MAX_PATH];
static HINSTANCE g_module_instance;

/* ── Executable Profiles (image base 0x400000) ─────────────────────── */

typedef struct MedgeNetExeProfile {
    const char *name;
    unsigned long exe_size;
    unsigned char sha256[32];
    unsigned long proto_http_set_secure;
    unsigned long proto_http_secure_flag;
    unsigned long proto_http_get;
    unsigned long proto_http_request;
    unsigned long proto_http_update;
    unsigned long fesl_push_hostname;
    unsigned long fesl_hostname_str;
    unsigned long fesl_packet_default;
    unsigned long fesl_load_paddr;
    unsigned long fesl_conn_vtable_tick;
    unsigned long fesl_conn_update;
    unsigned long process_tick;
    unsigned long g_network_obj;
    unsigned long proto_ssl_update;
    unsigned long locker_callback_func_slot;
    unsigned long locker_callback_userdata_slot;
    unsigned long locker_callback_expected_func;
    unsigned long fesl_session_global;
    unsigned long put_parse_branch_patch;
    unsigned long submit_txn_busy_guard;
    const unsigned char *expect_set_secure;
    unsigned int expect_set_secure_len;
    const unsigned char *expect_secure_flag;
    unsigned int expect_secure_flag_len;
    const unsigned char *expect_push_host;
    unsigned int expect_push_host_len;
    const unsigned char *expect_load_paddr;
    unsigned int expect_load_paddr_len;
    const unsigned char *expect_fesl_packet_default;
    unsigned int expect_fesl_packet_default_len;
    const unsigned char *expect_process_tick;
    unsigned int expect_process_tick_len;
    const unsigned char *expect_proto_http_get;
    unsigned int expect_proto_http_get_len;
    const unsigned char *expect_proto_http_request;
    unsigned int expect_proto_http_request_len;
} MedgeNetExeProfile;

static const MedgeNetExeProfile *g_profile;

/*
 * Stat-write deadlock fix: capture-and-call UE3 ProcessTick.
 *
 * During loading, the UE3 tick loop pauses.  FeslConnection::Tick continues
 * via a DirtySock timer at 62/sec.  We hook it and call ProcessTick (the UE3
 * online subsystem tick) which handles EVERYTHING: file locker tick (processes
 * the ghost PUT response), NetworkTick, UE3 ProcessEvent (fires delegates),
 * system handler dispatcher (calls SubmitTransaction on pending TXNs).
 *
 * ProcessTick's ECX (this) is captured via an inline hook during normal
 * gameplay.  During loading, we reuse the captured pointer.
 */
/* ProcessTick prologue (7 bytes):
 *   8B 41 C4           mov eax, [ecx-0x3C]
 *   D9 44 24 04        fld dword ptr [esp+4]
 */
static const unsigned char EXPECT_PROCESS_TICK[] = {
    0x8B, 0x41, 0xC4, 0xD9, 0x44, 0x24, 0x04
};

/* Expected original bytes for verification */
static const unsigned char EXPECT_SET_SECURE[]  = {
    0x8B, 0x4C, 0x24, 0x04, 0x33, 0xC0, 0x83, 0x7C
};
static const unsigned char EXPECT_SECURE_FLAG[] = {
    0x83, 0xC7, 0x01, 0x85, 0xED, 0x75, 0x0F, 0x8B
};
static const unsigned char EXPECT_PUSH_HOST_GOG[] = {
    0x68, 0x80, 0x83, 0xD8, 0x01
};
static const unsigned char EXPECT_PUSH_HOST_RETAIL[] = {
    0x68, 0x40, 0xA4, 0xD8, 0x01
};
static const unsigned char EXPECT_PUSH_HOST_DLC[] = {
    0x68, 0xD0, 0x0A, 0xD9, 0x01
};
static const unsigned char EXPECT_LOAD_PADDR[]  = { 0x8B, 0x40, 0x04, 0x0F, 0xB7, 0xC9 };
static const unsigned char EXPECT_FESL_PACKET_DEFAULT[] = {
    0xC7, 0x47, 0x0C, 0xA0, 0x1F, 0x00, 0x00
};
static const unsigned char EXPECT_PROTO_HTTP_GET[] = {
    0x8B, 0x4C, 0x24, 0x08, 0x33, 0xC0
};
static const unsigned char EXPECT_PROTO_HTTP_REQUEST[] = {
    0x8B, 0x4C, 0x24, 0x10, 0x8B, 0x44, 0x24, 0x04
};

/* Patch data */
static const unsigned char PATCH_SET_SECURE[]  = { 0xC3 };
static const unsigned char PATCH_SECURE_FLAG[] = { 0x31, 0xFF, 0x90 };
static const unsigned char PATCH_FESL_PACKET_DEFAULT[] = {
    0xC7, 0x47, 0x0C, 0x00, 0x00, 0x01, 0x00
};

static const MedgeNetExeProfile g_profiles[] = {
    {
        "GOG",
        31596544,
        {
            0x58, 0xDE, 0x3D, 0xF2, 0x1F, 0x40, 0xE9, 0x95,
            0x3E, 0x00, 0xBE, 0x92, 0xF7, 0x74, 0x90, 0x97,
            0x83, 0x2B, 0xAD, 0x02, 0xE5, 0xB6, 0x57, 0xD8,
            0x48, 0x5C, 0xDB, 0x61, 0xF3, 0x60, 0xA3, 0xDD
        },
        0x01664300, 0x0165E076, 0x0165E6D0, 0x0165E6F0,
        0x0165ED40, 0x015B9648, 0x01D88380, 0x015CCFD3,
        0x015B9640, 0x01D88368, 0x015B9680, 0x011C8DC0,
        0x0206E3F0, 0x01664380, 0x0206E4D0, 0x0206E4D4,
        0x0165D120, 0x0206E414, 0x0165CE47, 0x015E97BD,
        EXPECT_SET_SECURE, sizeof(EXPECT_SET_SECURE),
        EXPECT_SECURE_FLAG, sizeof(EXPECT_SECURE_FLAG),
        EXPECT_PUSH_HOST_GOG, sizeof(EXPECT_PUSH_HOST_GOG),
        EXPECT_LOAD_PADDR, sizeof(EXPECT_LOAD_PADDR),
        EXPECT_FESL_PACKET_DEFAULT, sizeof(EXPECT_FESL_PACKET_DEFAULT),
        EXPECT_PROCESS_TICK, sizeof(EXPECT_PROCESS_TICK)
        , EXPECT_PROTO_HTTP_GET, sizeof(EXPECT_PROTO_HTTP_GET),
        EXPECT_PROTO_HTTP_REQUEST, sizeof(EXPECT_PROTO_HTTP_REQUEST)
    },
    {
        "Steam",
        31946072,
        {
            0xA0, 0xF6, 0x53, 0xB6, 0x3B, 0x29, 0x9D, 0x5D,
            0x38, 0x99, 0xB8, 0xB4, 0xFA, 0x6E, 0x4D, 0x47,
            0xEE, 0xB3, 0x90, 0xBF, 0x26, 0xB1, 0xF0, 0x20,
            0x71, 0x04, 0x82, 0xEA, 0xBF, 0xE2, 0x97, 0xF0
        },
        0x01664300, 0x0165E076, 0x0165E6D0, 0x0165E6F0,
        0x0165ED40, 0x015B9648, 0x01D88380, 0x015CCFD3,
        0x015B9640, 0x01D88368, 0x015B9680, 0x011C8DC0,
        0x0206E3F0, 0x01664380, 0x0206E4D0, 0x0206E4D4,
        0x0165D120, 0x0206E414, 0x0165CE47, 0x015E97BD,
        EXPECT_SET_SECURE, sizeof(EXPECT_SET_SECURE),
        EXPECT_SECURE_FLAG, sizeof(EXPECT_SECURE_FLAG),
        EXPECT_PUSH_HOST_GOG, sizeof(EXPECT_PUSH_HOST_GOG),
        EXPECT_LOAD_PADDR, sizeof(EXPECT_LOAD_PADDR),
        EXPECT_FESL_PACKET_DEFAULT, sizeof(EXPECT_FESL_PACKET_DEFAULT),
        EXPECT_PROCESS_TICK, sizeof(EXPECT_PROCESS_TICK)
        , EXPECT_PROTO_HTTP_GET, sizeof(EXPECT_PROTO_HTTP_GET),
        EXPECT_PROTO_HTTP_REQUEST, sizeof(EXPECT_PROTO_HTTP_REQUEST)
    },
    {
        "EA App",
        31606704,
        {
            0xC2, 0x2F, 0xD2, 0x37, 0x8D, 0x90, 0xCC, 0x13,
            0x05, 0xA6, 0x5C, 0x60, 0x4B, 0xE1, 0x2D, 0x2A,
            0x33, 0x8F, 0x6A, 0x0C, 0x69, 0x3E, 0x31, 0x35,
            0xF7, 0x40, 0x2E, 0xB9, 0x17, 0xB0, 0x43, 0x7A
        },
        0x01664300, 0x0165E076, 0x0165E6D0, 0x0165E6F0,
        0x0165ED40, 0x015B9648, 0x01D88380, 0x015CCFD3,
        0x015B9640, 0x01D88368, 0x015B9680, 0x011C8DC0,
        0x0206E3F0, 0x01664380, 0x0206E4D0, 0x0206E4D4,
        0x0165D120, 0x0206E414, 0x0165CE47, 0x015E97BD,
        EXPECT_SET_SECURE, sizeof(EXPECT_SET_SECURE),
        EXPECT_SECURE_FLAG, sizeof(EXPECT_SECURE_FLAG),
        EXPECT_PUSH_HOST_GOG, sizeof(EXPECT_PUSH_HOST_GOG),
        EXPECT_LOAD_PADDR, sizeof(EXPECT_LOAD_PADDR),
        EXPECT_FESL_PACKET_DEFAULT, sizeof(EXPECT_FESL_PACKET_DEFAULT),
        EXPECT_PROCESS_TICK, sizeof(EXPECT_PROCESS_TICK)
        , EXPECT_PROTO_HTTP_GET, sizeof(EXPECT_PROTO_HTTP_GET),
        EXPECT_PROTO_HTTP_REQUEST, sizeof(EXPECT_PROTO_HTTP_REQUEST)
    },
    {
        "Retail",
        36484440,
        {
            0xC6, 0x69, 0x2E, 0x71, 0x95, 0x6E, 0x2C, 0xE8,
            0x6C, 0x93, 0xB1, 0x18, 0x16, 0xC9, 0x28, 0x0B,
            0xA7, 0x2E, 0x77, 0x14, 0x03, 0xA9, 0x26, 0x1A,
            0x6A, 0x51, 0x6F, 0x3A, 0xA1, 0xC7, 0xB5, 0x68
        },
        0x01665FE0, 0x0165FD56, 0x016603B0, 0x016603D0,
        0x01660A20, 0x015BB328, 0x01D8A440, 0x015CECB3,
        0x015BB320, 0x01D8A428, 0x015BB360, 0x011C9BD0,
        0x02087630, 0x01666060, 0x02087710, 0x02087714,
        0x0165EE00, 0x02087654, 0x0165EB27, 0x015EB49D,
        EXPECT_SET_SECURE, sizeof(EXPECT_SET_SECURE),
        EXPECT_SECURE_FLAG, sizeof(EXPECT_SECURE_FLAG),
        EXPECT_PUSH_HOST_RETAIL, sizeof(EXPECT_PUSH_HOST_RETAIL),
        EXPECT_LOAD_PADDR, sizeof(EXPECT_LOAD_PADDR),
        EXPECT_FESL_PACKET_DEFAULT, sizeof(EXPECT_FESL_PACKET_DEFAULT),
        EXPECT_PROCESS_TICK, sizeof(EXPECT_PROCESS_TICK)
        , EXPECT_PROTO_HTTP_GET, sizeof(EXPECT_PROTO_HTTP_GET),
        EXPECT_PROTO_HTTP_REQUEST, sizeof(EXPECT_PROTO_HTTP_REQUEST)
    },
    {
        "DLC Retail",
        37291352,
        {
            0x4D, 0x5E, 0xCC, 0x40, 0x88, 0x7A, 0x9A, 0x32,
            0x4F, 0xA8, 0x0C, 0x8F, 0xDC, 0x7A, 0x13, 0xE0,
            0xE7, 0xAF, 0xB8, 0xDA, 0x09, 0x0C, 0x05, 0x60,
            0xE3, 0x51, 0x7A, 0x21, 0xD1, 0x3C, 0x4B, 0xA9
        },
        0x0166A440, 0x016641B6, 0x01664810, 0x01664830,
        0x01664E80, 0x015BF788, 0x01D90AD0, 0x015D3113,
        0x015BF780, 0x01D90AB8, 0x015BF7C0, 0x011CC780,
        0x0208E750, 0x0166A4C0, 0x0208E830, 0x0208E834,
        0x01663260, 0x0208E774, 0x01662F87, 0x015EF8FD,
        EXPECT_SET_SECURE, sizeof(EXPECT_SET_SECURE),
        EXPECT_SECURE_FLAG, sizeof(EXPECT_SECURE_FLAG),
        EXPECT_PUSH_HOST_DLC, sizeof(EXPECT_PUSH_HOST_DLC),
        EXPECT_LOAD_PADDR, sizeof(EXPECT_LOAD_PADDR),
        EXPECT_FESL_PACKET_DEFAULT, sizeof(EXPECT_FESL_PACKET_DEFAULT),
        EXPECT_PROCESS_TICK, sizeof(EXPECT_PROCESS_TICK)
        , EXPECT_PROTO_HTTP_GET, sizeof(EXPECT_PROTO_HTTP_GET),
        EXPECT_PROTO_HTTP_REQUEST, sizeof(EXPECT_PROTO_HTTP_REQUEST)
    }
};

/* ── Logging ───────────────────────────────────────────────────────── */

static HANDLE g_log = INVALID_HANDLE_VALUE;
static unsigned int g_patch_failures;
static unsigned int g_patch_skips;

static void log_open(void)
{
    g_log = CreateFileA(g_log_path, GENERIC_WRITE, FILE_SHARE_READ,
                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void log_str(const char *s)
{
    DWORD n;
    const char *p = s;
    if (g_log == INVALID_HANDLE_VALUE) return;
    while (*p) p++;
    WriteFile(g_log, s, (DWORD)(p - s), &n, NULL);
}

static void log_hex(unsigned long v)
{
    char buf[11];
    const char hex[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    buf[2] = hex[(v >> 28) & 0xF]; buf[3] = hex[(v >> 24) & 0xF];
    buf[4] = hex[(v >> 20) & 0xF]; buf[5] = hex[(v >> 16) & 0xF];
    buf[6] = hex[(v >> 12) & 0xF]; buf[7] = hex[(v >>  8) & 0xF];
    buf[8] = hex[(v >>  4) & 0xF]; buf[9] = hex[v & 0xF];
    buf[10] = '\0';
    log_str(buf);
}

static void log_dec_ulong(unsigned long v)
{
    char buf[16];
    int i = 0, j;
    if (v == 0) {
        log_str("0");
        return;
    }
    while (v > 0 && i < (int)sizeof(buf) - 1) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (j = i - 1; j >= 0; j--) {
        char c[2];
        c[0] = buf[j];
        c[1] = '\0';
        log_str(c);
    }
}

static void log_sha256(const unsigned char digest[32])
{
    char buf[65];
    const char hex[] = "0123456789abcdef";
    unsigned int i;
    for (i = 0; i < 32; i++) {
        buf[i * 2] = hex[(digest[i] >> 4) & 0x0F];
        buf[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    buf[64] = '\0';
    log_str(buf);
}

static void log_line(const char *status, const char *name, unsigned long addr)
{
    if (status[1] == 'F') {
        g_patch_failures++;
        log_str("[FAIL] ");
    } else if (status[1] == 'S') {
        g_patch_skips++;
        log_str("[SKIP] ");
    } else {
        return;
    }
    log_str(name);
    log_str(" @ ");
    log_hex(addr);
    log_str("\r\n");
}

static void log_close(void)
{
    if (g_log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
    }
    g_log = INVALID_HANDLE_VALUE;
}

/* ── Helpers ───────────────────────────────────────────────────────── */

static void build_paths(void)
{
    char dir[MAX_PATH];
    char *slash, *p;

    GetModuleFileNameA(g_module_instance, dir, MAX_PATH);
    slash = dir;
    for (p = dir; *p; p++) {
        if (*p == '\\' || *p == '/') slash = p;
    }
    slash[1] = '\0';

    /* INI path */
    p = g_ini_path;
    { const char *s = dir; while (*s) *p++ = *s++; }
    { const char *s = INI_FILE; while (*s) *p++ = *s++; }
    *p = '\0';

    /* Log path */
    p = g_log_path;
    { const char *s = dir; while (*s) *p++ = *s++; }
    { const char *s = LOG_FILE; while (*s) *p++ = *s++; }
    *p = '\0';
}

static int verify_bytes(unsigned long addr, const unsigned char *expect, unsigned int len)
{
    const unsigned char *p = (const unsigned char *)addr;
    unsigned int i;
    if (IsBadReadPtr((void *)addr, len)) return 0;
    for (i = 0; i < len; i++) {
        if (p[i] != expect[i]) return 0;
    }
    return 1;
}

static int patch_bytes(unsigned long addr, const unsigned char *data, unsigned int len)
{
    DWORD old;
    unsigned int i;
    if (!VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    for (i = 0; i < len; i++)
        ((volatile unsigned char *)addr)[i] = data[i];
    VirtualProtect((void *)addr, len, old, &old);
    return 1;
}

typedef struct Sha256Ctx {
    unsigned long state[8];
    unsigned long bitcount_lo;
    unsigned long bitcount_hi;
    unsigned char buffer[64];
} Sha256Ctx;

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (SHA256_ROTR((x), 2) ^ SHA256_ROTR((x), 13) ^ SHA256_ROTR((x), 22))
#define SHA256_EP1(x) (SHA256_ROTR((x), 6) ^ SHA256_ROTR((x), 11) ^ SHA256_ROTR((x), 25))
#define SHA256_SIG0(x) (SHA256_ROTR((x), 7) ^ SHA256_ROTR((x), 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (SHA256_ROTR((x), 17) ^ SHA256_ROTR((x), 19) ^ ((x) >> 10))

static const unsigned long SHA256_K[64] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2
};

static void sha256_transform(Sha256Ctx *ctx, const unsigned char *data)
{
    unsigned long a, b, c, d, e, f, g, h, t1, t2, m[64];
    unsigned int i;

    for (i = 0; i < 16; i++) {
        m[i] = ((unsigned long)data[i * 4] << 24) |
               ((unsigned long)data[i * 4 + 1] << 16) |
               ((unsigned long)data[i * 4 + 2] << 8) |
               ((unsigned long)data[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++)
        m[i] = SHA256_SIG1(m[i - 2]) + m[i - 7] + SHA256_SIG0(m[i - 15]) + m[i - 16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + SHA256_EP1(e) + SHA256_CH(e, f, g) + SHA256_K[i] + m[i];
        t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(Sha256Ctx *ctx)
{
    ctx->bitcount_lo = 0;
    ctx->bitcount_hi = 0;
    ctx->state[0] = 0x6A09E667; ctx->state[1] = 0xBB67AE85;
    ctx->state[2] = 0x3C6EF372; ctx->state[3] = 0xA54FF53A;
    ctx->state[4] = 0x510E527F; ctx->state[5] = 0x9B05688C;
    ctx->state[6] = 0x1F83D9AB; ctx->state[7] = 0x5BE0CD19;
}

static void sha256_update(Sha256Ctx *ctx, const unsigned char *data, unsigned int len)
{
    unsigned int i, index, part_len;
    unsigned long old_lo;

    index = (unsigned int)((ctx->bitcount_lo >> 3) & 0x3F);
    old_lo = ctx->bitcount_lo;
    ctx->bitcount_lo += ((unsigned long)len << 3);
    if (ctx->bitcount_lo < old_lo)
        ctx->bitcount_hi++;
    ctx->bitcount_hi += ((unsigned long)len >> 29);

    part_len = 64 - index;
    if (len >= part_len) {
        for (i = 0; i < part_len; i++)
            ctx->buffer[index + i] = data[i];
        sha256_transform(ctx, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64)
            sha256_transform(ctx, data + i);
        index = 0;
    } else {
        i = 0;
    }

    while (i < len)
        ctx->buffer[index++] = data[i++];
}

static void sha256_final(Sha256Ctx *ctx, unsigned char digest[32])
{
    static const unsigned char padding[64] = { 0x80 };
    unsigned char bits[8];
    unsigned int i, index, pad_len;
    unsigned long hi = ctx->bitcount_hi;
    unsigned long lo = ctx->bitcount_lo;

    bits[0] = (unsigned char)(hi >> 24);
    bits[1] = (unsigned char)(hi >> 16);
    bits[2] = (unsigned char)(hi >> 8);
    bits[3] = (unsigned char)hi;
    bits[4] = (unsigned char)(lo >> 24);
    bits[5] = (unsigned char)(lo >> 16);
    bits[6] = (unsigned char)(lo >> 8);
    bits[7] = (unsigned char)lo;

    index = (unsigned int)((ctx->bitcount_lo >> 3) & 0x3F);
    pad_len = (index < 56) ? (56 - index) : (120 - index);
    sha256_update(ctx, padding, pad_len);
    sha256_update(ctx, bits, 8);

    for (i = 0; i < 8; i++) {
        digest[i * 4] = (unsigned char)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)ctx->state[i];
    }
}

static int sha256_file(HANDLE file, unsigned char digest[32])
{
    Sha256Ctx ctx;
    unsigned char buffer[1024];
    DWORD read_count;

    sha256_init(&ctx);
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &read_count, NULL))
            return 0;
        if (read_count == 0)
            break;
        sha256_update(&ctx, buffer, (unsigned int)read_count);
    }
    sha256_final(&ctx, digest);
    return 1;
}

static int same_sha256(const unsigned char *a, const unsigned char *b)
{
    unsigned int i;
    for (i = 0; i < 32; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static const MedgeNetExeProfile *find_exe_profile(void)
{
    char exe_path[MAX_PATH];
    unsigned char digest[32];
    HANDLE file;
    DWORD size_high;
    DWORD size_low;
    unsigned int i;
    unsigned int size_matches = 0;
    const MedgeNetExeProfile *size_match = NULL;

    if (!GetModuleFileNameA(NULL, exe_path, MAX_PATH))
        return NULL;


    file = CreateFileA(exe_path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        log_str("\r\nExecutable read: failed");
        return NULL;
    }

    size_high = 0;
    size_low = GetFileSize(file, &size_high);
    if (size_high != 0 || !sha256_file(file, digest)) {
        CloseHandle(file);
        log_str("\r\nExecutable hash: failed");
        return NULL;
    }
    CloseHandle(file);


    for (i = 0; i < sizeof(g_profiles) / sizeof(g_profiles[0]); i++) {
        if (g_profiles[i].exe_size != size_low)
            continue;
        size_match = &g_profiles[i];
        size_matches++;
        if (same_sha256(g_profiles[i].sha256, digest)) {
            return &g_profiles[i];
        }
    }

    if (size_matches == 1) {
        return size_match;
    }


    return NULL;
}

static int verify_profile_anchors(const MedgeNetExeProfile *profile)
{
    if (!profile)
        return 0;
    if (!verify_bytes(profile->proto_http_set_secure, profile->expect_set_secure,
                      profile->expect_set_secure_len))
        return 0;
    if (!verify_bytes(profile->proto_http_secure_flag, profile->expect_secure_flag,
                      profile->expect_secure_flag_len))
        return 0;
    if (!verify_bytes(profile->fesl_push_hostname, profile->expect_push_host,
                      profile->expect_push_host_len))
        return 0;
    if (!verify_bytes(profile->fesl_load_paddr, profile->expect_load_paddr,
                      profile->expect_load_paddr_len))
        return 0;
    if (!verify_bytes(profile->process_tick, profile->expect_process_tick,
                      profile->expect_process_tick_len))
        return 0;
    if (!verify_bytes(profile->proto_http_get, profile->expect_proto_http_get,
                      profile->expect_proto_http_get_len))
        return 0;
    if (!verify_bytes(profile->proto_http_request, profile->expect_proto_http_request,
                      profile->expect_proto_http_request_len))
        return 0;
    return 1;
}

static int write_jump_patch(unsigned long addr, void *target, unsigned int patch_len)
{
    unsigned char patch[16];
    unsigned int i;
    long rel;

    if (patch_len < 5 || patch_len > sizeof(patch))
        return 0;

    rel = (long)((unsigned long)target - (addr + 5));
    patch[0] = 0xE9;
    patch[1] = (unsigned char)(rel & 0xFF);
    patch[2] = (unsigned char)((rel >> 8) & 0xFF);
    patch[3] = (unsigned char)((rel >> 16) & 0xFF);
    patch[4] = (unsigned char)((rel >> 24) & 0xFF);
    for (i = 5; i < patch_len; i++)
        patch[i] = 0x90;

    return patch_bytes(addr, patch, patch_len);
}

static unsigned int cstr_len(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (unsigned int)(p - s);
}

static unsigned long module_image_size(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;

    if (!base || IsBadReadPtr(base, sizeof(IMAGE_DOS_HEADER)))
        return 0;

    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (IsBadReadPtr(nt, sizeof(IMAGE_NT_HEADERS)) ||
        nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    return nt->OptionalHeader.SizeOfImage;
}

static unsigned long find_module_pattern(const unsigned char *pattern, const char *mask)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    unsigned long size = module_image_size();
    unsigned int len = cstr_len(mask);
    unsigned long i;

    if (!base || size == 0 || len == 0 || size < len)
        return 0;

    for (i = 0; i <= size - len; i++) {
        unsigned int j;
        for (j = 0; j < len; j++) {
            if (mask[j] == 'x' && base[i + j] != pattern[j])
                break;
        }
        if (j == len)
            return (unsigned long)(base + i);
    }
    return 0;
}

static int install_trampoline_raw(unsigned long addr, unsigned int stolen_len,
                                  unsigned char *trampoline, void *detour)
{
    DWORD old;
    unsigned int i;
    long rel;

    if (!addr || stolen_len < 5 || stolen_len > 24)
        return 0;

    for (i = 0; i < stolen_len; i++)
        trampoline[i] = ((unsigned char *)addr)[i];

    trampoline[stolen_len] = 0xE9;
    rel = (long)((addr + stolen_len) - ((unsigned long)&trampoline[stolen_len] + 5));
    trampoline[stolen_len + 1] = (unsigned char)(rel & 0xFF);
    trampoline[stolen_len + 2] = (unsigned char)((rel >> 8) & 0xFF);
    trampoline[stolen_len + 3] = (unsigned char)((rel >> 16) & 0xFF);
    trampoline[stolen_len + 4] = (unsigned char)((rel >> 24) & 0xFF);

    if (!VirtualProtect((void *)trampoline, stolen_len + 5,
                        PAGE_EXECUTE_READWRITE, &old))
        return 0;

    return write_jump_patch(addr, detour, stolen_len);
}

static int install_trampoline(unsigned long addr, const unsigned char *expect,
                              unsigned int stolen_len, unsigned char *trampoline,
                              void *detour)
{
    DWORD old;
    unsigned int i;
    long rel;

    if (!verify_bytes(addr, expect, stolen_len))
        return 0;

    for (i = 0; i < stolen_len; i++)
        trampoline[i] = ((unsigned char *)addr)[i];

    trampoline[stolen_len] = 0xE9;
    rel = (long)((addr + stolen_len) - ((unsigned long)&trampoline[stolen_len] + 5));
    trampoline[stolen_len + 1] = (unsigned char)(rel & 0xFF);
    trampoline[stolen_len + 2] = (unsigned char)((rel >> 8) & 0xFF);
    trampoline[stolen_len + 3] = (unsigned char)((rel >> 16) & 0xFF);
    trampoline[stolen_len + 4] = (unsigned char)((rel >> 24) & 0xFF);

    if (!VirtualProtect((void *)trampoline, stolen_len + 5,
                        PAGE_EXECUTE_READWRITE, &old))
        return 0;

    return write_jump_patch(addr, detour, stolen_len);
}

static void log_reopen(void);

/* --- UE3 RestartLevel race fix -------------------------------------- */

#define UE_OBJECT_OUTER_OFFSET       0x28
#define UE_OBJECT_NAME_OFFSET        0x2C
#define UE_OBJECT_CLASS_OFFSET       0x34
#define UE_ACTOR_WORLDINFO_OFFSET    0x9C
#define UE_WORLDINFO_GAME_OFFSET     0x0CC4
#define UE_FNAME_ENTRY_WIDE_OFFSET   0x10
#define UE_PROCESS_EVENT_STOLEN_LEN  13
#define UE_MAX_OBJECT_SCAN           500000

typedef struct UEArray {
    void *data;
    int count;
    int max;
} UEArray;

typedef struct UEFName {
    int index;
    int number;
} UEFName;

typedef struct UEObject UEObject;
typedef struct UEFunction UEFunction;

static UEArray *g_ue_names;
static UEArray *g_ue_objects;
static UEFunction *g_ue_fn_restart_level;
static UEFunction *g_ue_fn_restart_race;
static unsigned char g_process_event_trampoline[32];
static unsigned long g_process_event_trampoline_addr;
static volatile LONG g_restart_fix_in_process_event;
static volatile unsigned long g_restart_fix_intercepts;
static volatile unsigned long g_restart_fix_function_resolves;
static volatile unsigned long g_restart_fix_resolve_attempts;

static int call_original_process_event(UEObject *object, UEFunction *function,
                                       void *params, void *result)
{
    int ret_value;
    __asm {
        mov  ecx, object
        push result
        push params
        push function
        call dword ptr [g_process_event_trampoline_addr]
        mov  ret_value, eax
    }
    return ret_value;
}

static int ue_array_valid(UEArray *array)
{
    if (!array || IsBadReadPtr(array, sizeof(UEArray)))
        return 0;
    if (array->count < 0 || array->count > UE_MAX_OBJECT_SCAN)
        return 0;
    if (array->max < array->count)
        return 0;
    if (array->count > 0 && (!array->data || IsBadReadPtr(array->data, array->count * 4)))
        return 0;
    return 1;
}

static void *ue_object_field(UEObject *object, unsigned long offset)
{
    if (!object || IsBadReadPtr((void *)((unsigned char *)object + offset), 4))
        return 0;
    return *(void **)((unsigned char *)object + offset);
}

static UEFName *ue_object_fname(UEObject *object)
{
    if (!object || IsBadReadPtr((void *)((unsigned char *)object + UE_OBJECT_NAME_OFFSET), sizeof(UEFName)))
        return 0;
    return (UEFName *)((unsigned char *)object + UE_OBJECT_NAME_OFFSET);
}

static int ue_wide_name_equals_ascii(const wchar_t *wide, const char *ascii)
{
    if (!wide || !ascii || IsBadReadPtr(wide, sizeof(wchar_t)))
        return 0;

    while (*ascii) {
        if (IsBadReadPtr(wide, sizeof(wchar_t)))
            return 0;
        if (*wide != (wchar_t)*ascii)
            return 0;
        wide++;
        ascii++;
    }

    if (IsBadReadPtr(wide, sizeof(wchar_t)))
        return 0;
    return *wide == 0;
}

static int ue_fname_equals_ascii(UEFName *name, const char *ascii)
{
    void **names_data;
    void *entry;
    wchar_t *wide_name;

    if (!ue_array_valid(g_ue_names) || !name || name->index < 0 ||
        name->index >= g_ue_names->count)
        return 0;

    names_data = (void **)g_ue_names->data;
    entry = names_data[name->index];
    if (!entry || IsBadReadPtr(entry, UE_FNAME_ENTRY_WIDE_OFFSET + sizeof(wchar_t)))
        return 0;

    wide_name = (wchar_t *)((unsigned char *)entry + UE_FNAME_ENTRY_WIDE_OFFSET);
    return ue_wide_name_equals_ascii(wide_name, ascii);
}

static int ue_object_name_equals(UEObject *object, const char *name)
{
    UEFName *fname = ue_object_fname(object);
    return fname && ue_fname_equals_ascii(fname, name);
}

static int ue_object_class_name_equals(UEObject *object, const char *class_name)
{
    UEObject *class_object = (UEObject *)ue_object_field(object, UE_OBJECT_CLASS_OFFSET);
    return class_object && ue_object_name_equals(class_object, class_name);
}

static UEObject *ue_object_outer(UEObject *object)
{
    return (UEObject *)ue_object_field(object, UE_OBJECT_OUTER_OFFSET);
}

static UEFunction *ue_find_function(const char *package_name,
                                    const char *class_name,
                                    const char *function_name)
{
    void **objects_data;
    int i;

    if (!ue_array_valid(g_ue_objects))
        return 0;

    objects_data = (void **)g_ue_objects->data;
    for (i = 0; i < g_ue_objects->count; i++) {
        UEObject *object = (UEObject *)objects_data[i];
        UEObject *outer_class;
        UEObject *outer_package;

        if (!object || IsBadReadPtr(object, UE_OBJECT_CLASS_OFFSET + 4))
            continue;
        if (!ue_object_class_name_equals(object, "Function"))
            continue;
        if (!ue_object_name_equals(object, function_name))
            continue;

        outer_class = ue_object_outer(object);
        outer_package = ue_object_outer(outer_class);
        if (!outer_class || !outer_package)
            continue;
        if (!ue_object_name_equals(outer_class, class_name))
            continue;
        if (!ue_object_name_equals(outer_package, package_name))
            continue;

        return (UEFunction *)object;
    }
    return 0;
}

static int ue_resolve_restart_functions(void)
{
    if ((!g_ue_fn_restart_level || !g_ue_fn_restart_race) &&
        g_restart_fix_resolve_attempts > 0 &&
        (g_restart_fix_resolve_attempts & 0xFF) != 0)
    {
        g_restart_fix_resolve_attempts++;
        return 0;
    }

    g_restart_fix_resolve_attempts++;

    if (!g_ue_fn_restart_level)
        g_ue_fn_restart_level =
            ue_find_function("Engine", "PlayerController", "RestartLevel");
    if (!g_ue_fn_restart_race)
        g_ue_fn_restart_race =
            ue_find_function("TdGame", "TdSPLevelRace", "RestartRace");

    if (g_ue_fn_restart_level && g_ue_fn_restart_race) {
        if (g_restart_fix_function_resolves == 0)
            g_restart_fix_function_resolves = 1;
        return 1;
    }
    return 0;
}

static UEObject *ue_current_level_race_game(UEObject *controller)
{
    UEObject *world;
    UEObject *game;

    if (!controller ||
        IsBadReadPtr((void *)((unsigned char *)controller + UE_ACTOR_WORLDINFO_OFFSET), 4))
        return 0;

    world = *(UEObject **)((unsigned char *)controller + UE_ACTOR_WORLDINFO_OFFSET);
    if (!world ||
        IsBadReadPtr((void *)((unsigned char *)world + UE_WORLDINFO_GAME_OFFSET), 4))
        return 0;

    game = *(UEObject **)((unsigned char *)world + UE_WORLDINFO_GAME_OFFSET);
    if (!game || !ue_object_class_name_equals(game, "TdSPLevelRace"))
        return 0;

    return game;
}

static void log_restart_fix_first_intercept(void)
{
    log_reopen();
    log_str("[OK]   RestartLevel redirected to TdSPLevelRace.RestartRace\r\n");
    log_close();
}

static int __fastcall detour_ProcessEvent(UEObject *object, void *edx,
                                          UEFunction *function,
                                          void *params, void *result)
{
    UEObject *race_game;
    unsigned char empty_params[4] = { 0, 0, 0, 0 };
    int ret_value;

    (void)edx;

    if (InterlockedCompareExchange(&g_restart_fix_in_process_event, 1, 0) == 0)
    {
        if (ue_resolve_restart_functions() && function == g_ue_fn_restart_level) {
            race_game = ue_current_level_race_game(object);
            if (race_game) {
                g_restart_fix_intercepts++;
                if (g_restart_fix_intercepts == 1)
                    log_restart_fix_first_intercept();

                ret_value = call_original_process_event(
                    race_game, g_ue_fn_restart_race, empty_params, result);
                InterlockedExchange(&g_restart_fix_in_process_event, 0);
                return ret_value;
            }
        }
        InterlockedExchange(&g_restart_fix_in_process_event, 0);
    }

    return call_original_process_event(object, function, params, result);
}

static int install_restartlevel_race_fix(void)
{
    unsigned long gnames_site;
    unsigned long gobjects_site;
    unsigned long process_event_site;

    gnames_site = find_module_pattern(
        (const unsigned char *)"\x8B\x0D\x00\x00\x00\x00\x8B\x84\x24\x00\x00\x00\x00\x8B\x04\x81",
        "xx????xxx????xxx");
    if (!gnames_site) {
        log_line("[SKIP] ", "RestartLevel race fix GNames pattern", 0);
        return 0;
    }
    g_ue_names = *(UEArray **)(gnames_site + 2);

    gobjects_site = find_module_pattern(
        (const unsigned char *)"\x8B\x15\x00\x00\x00\x00\x8B\x0C\xB2\x8D\x44\x24\x30",
        "xx????xxxxxxx");
    if (!gobjects_site) {
        log_line("[SKIP] ", "RestartLevel race fix GObjects pattern", 0);
        return 0;
    }
    g_ue_objects = *(UEArray **)(gobjects_site + 2);

    if (!ue_array_valid(g_ue_names) || !ue_array_valid(g_ue_objects)) {
        log_line("[SKIP] ", "RestartLevel race fix reflection globals", 0);
        return 0;
    }

    process_event_site = find_module_pattern(
        (const unsigned char *)"\x56\x8B\xF1\x8B\x0D\x00\x00\x00\x00\x85\xC9\x74\x09",
        "xxxxx????xxxx");
    if (!process_event_site) {
        log_line("[SKIP] ", "RestartLevel race fix ProcessEvent pattern", 0);
        return 0;
    }

    if (!install_trampoline_raw(process_event_site, UE_PROCESS_EVENT_STOLEN_LEN,
                                g_process_event_trampoline, detour_ProcessEvent)) {
        log_line("[FAIL] ", "Hook ProcessEvent for RestartLevel race fix",
                 process_event_site);
        return 0;
    }
    g_process_event_trampoline_addr = (unsigned long)&g_process_event_trampoline[0];

    log_str("[OK]   Hook ProcessEvent for RestartLevel race fix @ ");
    log_hex(process_event_site);
    log_str("\r\n");
    ue_resolve_restart_functions();
    return 1;
}

/* ── Config ────────────────────────────────────────────────────────── */

static void read_config(void)
{
    char port_buf[16];
    char http_port_buf[16];

    GetPrivateProfileStringA(INI_SECTION, "Host", DEFAULT_HOST,
                             g_host, MAX_HOST_LEN, g_ini_path);

    GetPrivateProfileStringA(INI_SECTION, "Port", "18680",
                             port_buf, sizeof(port_buf), g_ini_path);

    g_port = 0;
    { const char *p = port_buf; while (*p >= '0' && *p <= '9') { g_port = g_port * 10 + (*p - '0'); p++; } }
    if (g_port == 0) g_port = DEFAULT_PORT;

    GetPrivateProfileStringA(INI_SECTION, "HTTPPort", "80",
                             http_port_buf, sizeof(http_port_buf), g_ini_path);

    g_http_port = 0;
    { const char *p = http_port_buf; while (*p >= '0' && *p <= '9') { g_http_port = g_http_port * 10 + (*p - '0'); p++; } }
    if (g_http_port == 0) g_http_port = DEFAULT_HTTP_PORT;
}

static void log_reopen(void);

typedef int (__cdecl *ProtoHttpGet_t)(void *httpRef, const char *url, int headOnly);
typedef int (__cdecl *ProtoHttpRequest_t)(void *httpRef, const char *url,
                                          const void *body, int bodyLen, int doPut);

static unsigned char g_proto_http_get_trampoline[16];
static unsigned char g_proto_http_request_trampoline[16];
static ProtoHttpGet_t g_ProtoHttpGet = (ProtoHttpGet_t)g_proto_http_get_trampoline;
static ProtoHttpRequest_t g_ProtoHttpRequest = (ProtoHttpRequest_t)g_proto_http_request_trampoline;

static int str_starts_with(const char *value, const char *prefix)
{
    while (*prefix) {
        if (*value++ != *prefix++)
            return 0;
    }
    return 1;
}

static char *append_text(char *dst, char *end, const char *src)
{
    while (*src && dst < end)
        *dst++ = *src++;
    return dst;
}

static char *append_uint(char *dst, char *end, unsigned int value)
{
    char tmp[12];
    unsigned int n = 0, i;

    if (value == 0) {
        if (dst < end) *dst++ = '0';
        return dst;
    }

    while (value > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    for (i = 0; i < n && dst < end; i++)
        *dst++ = tmp[n - 1 - i];
    return dst;
}

static int rewrite_easo_url(const char *url, char *out, unsigned int out_len)
{
    const char *path;
    char *p, *end;

    if (!url || !out || out_len == 0)
        return 0;

    if (str_starts_with(url, "https://easo.ea.com"))
        path = url + 19;
    else if (str_starts_with(url, "http://easo.ea.com"))
        path = url + 18;
    else
        return 0;

    if (*path == '\0')
        path = "/";

    p = out;
    end = out + out_len - 1;
    p = append_text(p, end, "http://");
    p = append_text(p, end, g_host);
    if (g_http_port != 80) {
        if (p < end) *p++ = ':';
        p = append_uint(p, end, (unsigned int)g_http_port);
    }
    p = append_text(p, end, path);
    *p = '\0';

    return 1;
}

static int __cdecl detour_ProtoHttpGet(void *httpRef, const char *url, int headOnly)
{
    char rewritten[MAX_URL_LEN];
    if (rewrite_easo_url(url, rewritten, sizeof(rewritten))) {
        return g_ProtoHttpGet(httpRef, rewritten, headOnly);
    }
    return g_ProtoHttpGet(httpRef, url, headOnly);
}

static int __cdecl detour_ProtoHttpRequest(void *httpRef, const char *url,
                                           const void *body, int bodyLen, int doPut)
{
    char rewritten[MAX_URL_LEN];
    if (rewrite_easo_url(url, rewritten, sizeof(rewritten))) {
        return g_ProtoHttpRequest(httpRef, rewritten, body, bodyLen, doPut);
    }
    return g_ProtoHttpRequest(httpRef, url, body, bodyLen, doPut);
}

static void install_protohttp_url_hooks(void)
{
    int ok;
    const MedgeNetExeProfile *profile = g_profile;

    if (!profile)
        return;

    ok = install_trampoline(profile->proto_http_get, profile->expect_proto_http_get,
                            profile->expect_proto_http_get_len,
                            g_proto_http_get_trampoline,
                            detour_ProtoHttpGet);
    log_line(ok ? "[OK]   " : "[SKIP] ", "Hook ProtoHttpGet URL rewrite",
             profile->proto_http_get);

    ok = install_trampoline(profile->proto_http_request, profile->expect_proto_http_request,
                            profile->expect_proto_http_request_len,
                            g_proto_http_request_trampoline,
                            detour_ProtoHttpRequest);
    log_line(ok ? "[OK]   " : "[SKIP] ", "Hook ProtoHttpRequest URL rewrite",
             profile->proto_http_request);
}

/* ── ProcessTick Capture Hook ──────────────────────────────────────── */

static volatile unsigned long g_process_tick_ecx = 0;
static volatile unsigned long g_pt_counter = 0;
static volatile long g_in_process_tick = 0;
static unsigned long g_orig_FeslConnUpdate_addr = 0;
static unsigned long g_network_obj_addr = 0;

typedef void (__cdecl *ProtoHttpUpdate_t)(void *httpRef);
static ProtoHttpUpdate_t g_ProtoHttpUpdate = (ProtoHttpUpdate_t)0;
typedef void (__cdecl *LockerCallback_t)(void *locker, int result, void *response, void *userData);

static void log_reopen(void);

static void tick_http_locker(void)
{
    unsigned long func, locker, httpHandle, httpState;
    const MedgeNetExeProfile *profile = g_profile;

    if (!profile)
        return;

    if (IsBadReadPtr((void *)profile->locker_callback_func_slot, 4)) return;
    func = *(unsigned long *)profile->locker_callback_func_slot;
    if (func != profile->locker_callback_expected_func) return;

    if (IsBadReadPtr((void *)profile->locker_callback_userdata_slot, 4)) return;
    locker = *(unsigned long *)profile->locker_callback_userdata_slot;
    if (!locker || IsBadReadPtr((void *)locker, 4)) return;

    httpHandle = *(unsigned long *)locker;
    if (!httpHandle || IsBadReadPtr((void *)(httpHandle + 0x44), 4)) return;

    httpState = *(unsigned long *)(httpHandle + 0x18);
    if (httpState <= 2) {
        *(unsigned long *)(httpHandle + 0x44) = 0;
    }

    g_ProtoHttpUpdate((void *)httpHandle);
}

static volatile unsigned long g_locker_prev_state = 0;
static volatile unsigned long g_locker_put_success_replays = 0;

static void diag_locker_state(void)
{
    unsigned long locker, state;
    const MedgeNetExeProfile *profile = g_profile;

    if (!profile)
        return;

    if (IsBadReadPtr((void *)profile->locker_callback_userdata_slot, 4)) return;
    locker = *(unsigned long *)profile->locker_callback_userdata_slot;
    if (!locker || IsBadReadPtr((void *)(locker + 0x1DE30), 4)) return;

    state = *(unsigned long *)(locker + 0x1DE30);
    if (state == 0 && g_locker_prev_state != 0) {
        unsigned long err = 0, lastResult = 0;
        unsigned long callback = 0, userData = 0, responseData;
        int replayed = 0;
        if (!IsBadReadPtr((void *)(locker + 0x1DDEC), 4))
            err = *(unsigned long *)(locker + 0x1DDEC);
        if (!IsBadReadPtr((void *)(locker + 0x1DDE8), 4))
            lastResult = *(unsigned long *)(locker + 0x1DDE8);
        if (!IsBadReadPtr((void *)(locker + 0x1DDC4), 4))
            callback = *(unsigned long *)(locker + 0x1DDC4);
        if (!IsBadReadPtr((void *)(locker + 0x1DDC8), 4))
            userData = *(unsigned long *)(locker + 0x1DDC8);
        responseData = locker + 0x1DC1C;

        if (g_locker_prev_state == 5 && lastResult == 4 && callback &&
            !IsBadReadPtr((void *)callback, 1))
        {
            LockerCallback_t cb = (LockerCallback_t)callback;
            cb((void *)locker, 1, (void *)responseData, (void *)userData);
            g_locker_put_success_replays++;
            replayed = 1;
        }

        if (err != 0 || lastResult == 0xFFFFFFFF) {
            log_reopen();
            log_str("[WARN] FileLocker PUT completion");
            log_str(" result="); log_hex(lastResult);
            log_str(" err="); log_hex(err);
            log_str("\r\n");
            log_close();
        }

        g_locker_prev_state = 0;
        return;
    }
    g_locker_prev_state = state;
}

/* Trampoline: execute stolen 7-byte prologue, then JMP to original+7.
 * Must be PAGE_EXECUTE_READWRITE (set in install function). */
static unsigned char g_pt_trampoline[16];

/* Capture stub: saves ECX, increments counter, then jumps to trampoline */
__declspec(naked) static void capture_ProcessTick(void)
{
    __asm {
        mov  dword ptr [g_process_tick_ecx], ecx
        inc  dword ptr [g_pt_counter]
        jmp  dword ptr [g_pt_trampoline + 12]
    }
}

static int install_process_tick_hook(void)
{
    const MedgeNetExeProfile *profile = g_profile;
    unsigned long target;
    unsigned long capture_addr = (unsigned long)capture_ProcessTick;
    unsigned char jmp_patch[7];
    long rel;
    DWORD old;

    if (!profile)
        return 0;

    target = profile->process_tick;
    if (!verify_bytes(target, profile->expect_process_tick, profile->expect_process_tick_len))
        return 0;

    /* Build trampoline: stolen 7 bytes + JMP back to original+7 */
    g_pt_trampoline[0]  = 0x8B;  /* mov eax, [ecx-0x3C] */
    g_pt_trampoline[1]  = 0x41;
    g_pt_trampoline[2]  = 0xC4;
    g_pt_trampoline[3]  = 0xD9;  /* fld dword ptr [esp+4] */
    g_pt_trampoline[4]  = 0x44;
    g_pt_trampoline[5]  = 0x24;
    g_pt_trampoline[6]  = 0x04;
    g_pt_trampoline[7]  = 0xE9;  /* jmp rel32 back to original+7 */
    {
        unsigned long jmp_addr = (unsigned long)&g_pt_trampoline[7];
        long back_rel = (long)((target + 7) - (jmp_addr + 5));
        g_pt_trampoline[8]  = (unsigned char)(back_rel & 0xFF);
        g_pt_trampoline[9]  = (unsigned char)((back_rel >> 8) & 0xFF);
        g_pt_trampoline[10] = (unsigned char)((back_rel >> 16) & 0xFF);
        g_pt_trampoline[11] = (unsigned char)((back_rel >> 24) & 0xFF);
    }
    /* Store trampoline entry address for the capture stub's indirect JMP */
    *(unsigned long *)&g_pt_trampoline[12] = (unsigned long)&g_pt_trampoline[0];

    if (!VirtualProtect((void *)g_pt_trampoline, sizeof(g_pt_trampoline),
                        PAGE_EXECUTE_READWRITE, &old))
        return 0;

    /* Patch ProcessTick prologue: JMP capture_stub + 2 NOPs */
    rel = (long)(capture_addr - (target + 5));
    jmp_patch[0] = 0xE9;
    jmp_patch[1] = (unsigned char)(rel & 0xFF);
    jmp_patch[2] = (unsigned char)((rel >> 8) & 0xFF);
    jmp_patch[3] = (unsigned char)((rel >> 16) & 0xFF);
    jmp_patch[4] = (unsigned char)((rel >> 24) & 0xFF);
    jmp_patch[5] = 0x90;
    jmp_patch[6] = 0x90;

    return patch_bytes(target, jmp_patch, 7);
}

/* ── FeslConnection::Tick Detour ──────────────────────────────────── */

/* IEEE 754 for 0.016f (approx 1/60s) */
#define FLOAT_16MS 0x3C83126F

/*
 * Stale-tick detection: g_pt_counter is incremented by the capture stub
 * on every UE3 ProcessTick call.  g_pt_last_seen tracks the last value
 * we observed.  g_pt_stale counts how many transport ticks passed with
 * no ProcessTick activity.  After ~30 stale ticks (~0.5s), we conclude
 * the UE3 tick is paused (loading scene) and start calling ProcessTick.
 */
static unsigned long g_pt_last_seen = 0;
static unsigned long g_pt_stale = 0;
static volatile unsigned long g_pt_max_stale = 0;
static volatile unsigned long g_pt_calls_as_stale = 0;

#define PT_STALE_THRESHOLD 30

__declspec(naked) static void detour_FeslConnUpdate(void)
{
    __asm {
        push ebp
        mov  ebp, esp
        push esi
        mov  esi, ecx

        cmp  dword ptr [g_in_process_tick], 0
        jne  _reentrant

        /* Clear GOSManager+0x1D4 busy flag on EVERY tick.
         * This flag is left at 4 after stat reads complete.
         * GOSManager_SubmitTxn only proceeds when it's 0.
         * The ghost save callback fires within ProcessTick (which runs
         * every frame), so the flag must be 0 BEFORE that frame starts.
         * Our transport tick fires between frames, clearing it just in time. */
        mov  eax, dword ptr [g_network_obj_addr]
        mov  eax, [eax]
        test eax, eax
        jz   _skip_clear
        mov  byte ptr [eax + 0x1D4], 0
    _skip_clear:

        call tick_http_locker
        call diag_locker_state

        /* Update stale-tick counter */
        mov  eax, dword ptr [g_pt_counter]
        cmp  eax, dword ptr [g_pt_last_seen]
        je   _counter_stale
        mov  dword ptr [g_pt_last_seen], eax
        mov  dword ptr [g_pt_stale], 0
        jmp  _just_original
    _counter_stale:
        inc  dword ptr [g_pt_stale]
        mov  eax, dword ptr [g_pt_stale]
        cmp  eax, dword ptr [g_pt_max_stale]
        jbe  _no_max_update
        mov  dword ptr [g_pt_max_stale], eax
    _no_max_update:

        /* Only call ProcessTick after sustained stall */
        cmp  dword ptr [g_pt_stale], PT_STALE_THRESHOLD
        jb   _just_original
        inc  dword ptr [g_pt_calls_as_stale]

        /* Need captured ECX */
        cmp  dword ptr [g_process_tick_ecx], 0
        je   _just_original

        mov  dword ptr [g_in_process_tick], 1
        push FLOAT_16MS
        mov  ecx, dword ptr [g_process_tick_ecx]
        call dword ptr [g_pt_trampoline + 12]
        add  esp, 4

        mov  dword ptr [g_in_process_tick], 0
        jmp  _done

    _just_original:
        push dword ptr [ebp+8]
        mov  ecx, esi
        call [g_orig_FeslConnUpdate_addr]
        jmp  _done

    _reentrant:
        push dword ptr [ebp+8]
        mov  ecx, esi
        call [g_orig_FeslConnUpdate_addr]

    _done:
        pop  esi
        pop  ebp
        ret  4
    }
}

/* ── Patches ───────────────────────────────────────────────────────── */

static void apply_patches(void)
{
    int ok;
    const MedgeNetExeProfile *profile = g_profile;

    if (!profile)
        return;

    /* Patch 0: Raise FESL app packet limit from 0x1FA0 to 0x10000.
     *
     * FeslConn allocates ProtoSSL buffers as configMax + 0x80, and the
     * ProtoSSL receive check compares packet length against that allocation.
     * Patching the config default keeps allocation and validation in sync.
     */
    if (verify_bytes(profile->fesl_packet_default, profile->expect_fesl_packet_default,
                     profile->expect_fesl_packet_default_len)) {
        ok = patch_bytes(profile->fesl_packet_default, PATCH_FESL_PACKET_DEFAULT,
                         sizeof(PATCH_FESL_PACKET_DEFAULT));
        log_line(ok ? "[OK]   " : "[FAIL] ", "Raise FESL packet limit",
                 profile->fesl_packet_default);
    } else {
        log_line("[SKIP] ", "Raise FESL packet limit (verify failed)",
                 profile->fesl_packet_default);
    }

    /* Patch 1: NOP ProtoHttpSetSecure -> ret */
    if (verify_bytes(profile->proto_http_set_secure, profile->expect_set_secure,
                     profile->expect_set_secure_len)) {
        ok = patch_bytes(profile->proto_http_set_secure, PATCH_SET_SECURE,
                         sizeof(PATCH_SET_SECURE));
        log_line(ok ? "[OK]   " : "[FAIL] ", "NOP ProtoHttpSetSecure",
                 profile->proto_http_set_secure);
    } else {
        log_line("[SKIP] ", "NOP ProtoHttpSetSecure (verify failed)",
                 profile->proto_http_set_secure);
    }

    /* Patch 2: Force ProtoHTTP secure flag to 0 */
    if (verify_bytes(profile->proto_http_secure_flag, profile->expect_secure_flag,
                     profile->expect_secure_flag_len)) {
        ok = patch_bytes(profile->proto_http_secure_flag, PATCH_SECURE_FLAG,
                         sizeof(PATCH_SECURE_FLAG));
        log_line(ok ? "[OK]   " : "[FAIL] ", "Force secure=0",
                 profile->proto_http_secure_flag);
    } else {
        log_line("[SKIP] ", "Force secure=0 (verify failed)",
                 profile->proto_http_secure_flag);
    }

    install_protohttp_url_hooks();

    /* Patch 3: Redirect FESL hostname push to our buffer */
    if (verify_bytes(profile->fesl_push_hostname, profile->expect_push_host,
                     profile->expect_push_host_len)) {
        /*
         * Original: push <profile fesl.ea.com string>
         * Patched:  68 XX XX XX XX    push <address of g_host>
         *
         * Redirects the pDefHost parameter of ProtoSSL_Connect.
         */
        unsigned char push_patch[5];
        unsigned long host_addr = (unsigned long)g_host;
        push_patch[0] = 0x68;
        push_patch[1] = (unsigned char)(host_addr & 0xFF);
        push_patch[2] = (unsigned char)((host_addr >> 8) & 0xFF);
        push_patch[3] = (unsigned char)((host_addr >> 16) & 0xFF);
        push_patch[4] = (unsigned char)((host_addr >> 24) & 0xFF);

        ok = patch_bytes(profile->fesl_push_hostname, push_patch, 5);
        log_line(ok ? "[OK]   " : "[FAIL] ", "Redirect FESL push",
                 profile->fesl_push_hostname);
    } else {
        log_line("[SKIP] ", "Redirect FESL push (verify failed)",
                 profile->fesl_push_hostname);
    }

    /* Patch 4: Overwrite the static "fesl.ea.com" string in .rdata
     *
     * ProtoSSL_Connect receives BOTH pDefHost (the push we patched above)
     * AND pAddr (from FeslConfig override buffer, passed via [params+4]).
     * When the override is empty, ProtoSSL falls back to using the string
     * at the profile's fesl.ea.com string for DNS resolution. Overwriting it
     * ensures ALL code paths that reference this string get our hostname.
     */
    {
        unsigned char host_buf[12];
        unsigned int i;
        for (i = 0; i < 12; i++) host_buf[i] = 0;
        for (i = 0; i < 11 && g_host[i]; i++) host_buf[i] = (unsigned char)g_host[i];

        ok = patch_bytes(profile->fesl_hostname_str, host_buf, 12);
        log_line(ok ? "[OK]   " : "[FAIL] ", "Overwrite fesl.ea.com string",
                 profile->fesl_hostname_str);
    }

    /* Patch 5: Force pAddr to point to static hostname string */
    if (verify_bytes(profile->fesl_load_paddr, profile->expect_load_paddr,
                     profile->expect_load_paddr_len)) {
        unsigned char paddr_patch[6];
        unsigned long paddr_target = (unsigned long)g_host;
        paddr_patch[0] = 0xB8;
        paddr_patch[1] = (unsigned char)(paddr_target & 0xFF);
        paddr_patch[2] = (unsigned char)((paddr_target >> 8) & 0xFF);
        paddr_patch[3] = (unsigned char)((paddr_target >> 16) & 0xFF);
        paddr_patch[4] = (unsigned char)((paddr_target >> 24) & 0xFF);
        paddr_patch[5] = 0x90;

        ok = patch_bytes(profile->fesl_load_paddr, paddr_patch, 6);
        log_line(ok ? "[OK]   " : "[FAIL] ", "Force pAddr to hostname",
                 profile->fesl_load_paddr);
    } else {
        log_line("[SKIP] ", "Force pAddr (verify failed)", profile->fesl_load_paddr);
    }

    /* Patch 6: Force PUT handler to always proceed to parse.
     *
     * The EASO locker PUT handler checks
     * bodyReceived < bodyTotal before parsing the response XML.
     * bodyReceived is advanced by a progress callback that defaults to
     * a no-op stub.  Without a real callback, bodyReceived
     * never advances and the handler returns without parsing.
     * Changing jge (0x7D) to jmp (0xEB) at the profile site forces parsing.
     */
    {
        static const unsigned char expect_jge = 0x7D;
        static const unsigned char patch_jmp  = 0xEB;
        if (verify_bytes(profile->put_parse_branch_patch, &expect_jge, 1)) {
            ok = patch_bytes(profile->put_parse_branch_patch, &patch_jmp, 1);
            log_line(ok ? "[OK]   " : "[FAIL] ",
                     "Force PUT handler parse", profile->put_parse_branch_patch);
        } else {
            log_line("[SKIP] ",
                     "Force PUT handler parse (verify failed)",
                     profile->put_parse_branch_patch);
        }
    }

    /* Patch 6b removed: the iVar2==4 check at 0x0165CE58 is needed.
     * fcn.0165c4b0() returns 4 once the body is fully read. */

    /* Patch 7: NOP the busy-flag guard in GOSManager_SubmitTxn.
     *
     * GOSManager_SubmitTxn checks a byte at GOSManager+0x1D4.
     * If non-zero, it skips the entire TXN preparation.  The flag is left
     * at 4 ("completed") after stat reads and is only reset by a tick that
     * runs within ProcessTick's handler dispatcher.  When FlushOnlineStats
     * fires from the ghost save callback (ALSO within ProcessTick), the
     * handler dispatcher has already re-set the flag to 4 earlier in the
     * same frame.  NOPing the guard lets SubmitTxn always proceed.
     *
     *   Profile site: 0F 85 B6 00 00 00   jne skip_all  (6 bytes)
     *   Patched to: 90 90 90 90 90 90   nop x6
     */
    {
        static const unsigned char expect_jne[] = { 0x0F, 0x85, 0xB6, 0x00, 0x00, 0x00 };
        static const unsigned char patch_nop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        if (verify_bytes(profile->submit_txn_busy_guard, expect_jne, 6)) {
            ok = patch_bytes(profile->submit_txn_busy_guard, patch_nop6, 6);
            log_line(ok ? "[OK]   " : "[FAIL] ",
                     "NOP SubmitTxn busy guard", profile->submit_txn_busy_guard);
        } else {
            log_line("[SKIP] ",
                     "NOP SubmitTxn busy guard (verify failed)",
                     profile->submit_txn_busy_guard);
        }
    }

    /* Patch 7: Capture ProcessTick ECX via inline hook */
    {
        ok = install_process_tick_hook();
        log_line(ok ? "[OK]   " : "[SKIP] ",
                 "Capture ProcessTick", profile->process_tick);
    }

    install_restartlevel_race_fix();

    /* Patch 7: Hook FeslConnection::Tick vtable to call ProcessTick
     * during loading, replacing the paused UE3 tick loop.
     */
    {
        unsigned long vtable_slot = profile->fesl_conn_vtable_tick;
        unsigned long expected_fn = profile->fesl_conn_update;

        if (!IsBadReadPtr((void *)vtable_slot, 4) &&
            *(unsigned long *)vtable_slot == expected_fn)
        {
            unsigned long new_fn = (unsigned long)detour_FeslConnUpdate;
            ok = patch_bytes(vtable_slot, (unsigned char *)&new_fn, 4);
            log_line(ok ? "[OK]   " : "[FAIL] ",
                     "Hook FeslConnUpdate vtable", vtable_slot);
        } else {
            log_line("[SKIP] ",
                     "Hook FeslConnUpdate vtable (verify failed)", vtable_slot);
        }
    }
}

/* ── Delayed heap patch (background thread) ────────────────────────── */

#define FESL_HOST_OVERRIDE_OFFSET 0xD8

static void log_reopen(void)
{
    g_log = CreateFileA(g_log_path, GENERIC_WRITE, FILE_SHARE_READ,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log != INVALID_HANDLE_VALUE)
        SetFilePointer(g_log, 0, NULL, FILE_END);
}

static void probe_txn_pointers(void)
{
    log_reopen();
    log_str("\r\n[DIAG] pt_counter="); log_hex(g_pt_counter);
    log_str(" max_stale="); log_hex(g_pt_max_stale);
    log_str(" calls_stale="); log_hex(g_pt_calls_as_stale);
    log_str(" captured_ecx="); log_hex(g_process_tick_ecx);
    log_str(" restart_resolved="); log_hex(g_restart_fix_function_resolves);
    log_str(" restart_intercepts="); log_hex(g_restart_fix_intercepts);
    log_str("\r\n");
    log_close();
}

static DWORD WINAPI patch_heap_thread(LPVOID param)
{
    const MedgeNetExeProfile *profile = g_profile;
    volatile unsigned long *session_ptr;
    unsigned long session;
    unsigned char *override_buf;
    unsigned int i;
    int attempts = 0;

    (void)param;
    if (!profile)
        return 1;

    session_ptr = (volatile unsigned long *)profile->fesl_session_global;

    while (attempts < 120) {
        Sleep(500);
        attempts++;

        if (IsBadReadPtr((void *)profile->fesl_session_global, 4))
            continue;

        session = *session_ptr;
        if (session == 0)
            continue;

        override_buf = (unsigned char *)(session + FESL_HOST_OVERRIDE_OFFSET);
        if (IsBadWritePtr((void *)override_buf, MAX_HOST_LEN))
            continue;

        for (i = 0; i < MAX_HOST_LEN - 1 && g_host[i]; i++)
            override_buf[i] = (unsigned char)g_host[i];
        override_buf[i] = 0;

        log_reopen();
        log_str("[OK]   Game session connected to ");
        log_str(g_host);
        log_str("\r\n");
        log_close();

        return 0;
    }

    log_reopen();
    log_str("[FAIL] FeslHostOverride: FeslSession not found after 60s\r\n");
    log_close();
    return 1;
}

/* ── Entry Point ───────────────────────────────────────────────────── */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;

    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_module_instance = hinstDLL;
        build_paths();
        read_config();
        log_open();

        log_str("MedgeNet Client ");
        log_str(MEDGENET_VERSION_STRING);
        log_str("\r\nServer: ");
        log_str(g_host);
        log_str(":");
        log_dec_ulong((unsigned long)g_port);
        log_str(" (HTTP ");
        log_dec_ulong((unsigned long)g_http_port);
        log_str(")\r\n");

        g_profile = find_exe_profile();
        if (!g_profile) {
            log_str("\r\n[FAIL] Unsupported Mirror's Edge executable. No patches applied.\r\n");
            log_close();
            return TRUE;
        }
        if (!verify_profile_anchors(g_profile)) {
            log_str("\r\n[FAIL] Profile verification failed for ");
            log_str(g_profile->name);
            log_str(". No patches applied.\r\n");
            log_close();
            return TRUE;
        }

        g_orig_FeslConnUpdate_addr = g_profile->fesl_conn_update;
        g_network_obj_addr = g_profile->g_network_obj;
        g_ProtoHttpUpdate = (ProtoHttpUpdate_t)g_profile->proto_http_update;

        log_str("\r\nProfile: ");
        log_str(g_profile->name);
        log_str("\r\n");

        g_patch_failures = 0;
        g_patch_skips = 0;
        apply_patches();

        log_str("Patches: ");
        if (g_patch_failures == 0 && g_patch_skips == 0) {
            log_str("OK\r\nWaiting for game session...\r\n");
        } else {
            log_str("completed with ");
            log_dec_ulong(g_patch_failures);
            log_str(" failed, ");
            log_dec_ulong(g_patch_skips);
            log_str(" skipped\r\n");
        }
        log_close();

        CreateThread(NULL, 0, patch_heap_thread, NULL, 0, NULL);
    }

    return TRUE;
}
