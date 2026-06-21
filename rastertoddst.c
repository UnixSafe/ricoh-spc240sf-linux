/*
 * rastertoddst — CUPS filter for the Ricoh Aficio SP C240SF (DDST/GDI).
 *
 * Native C implementation. Reads application/vnd.cups-raster from stdin,
 * halftones each page to 1-bit per channel via an ordered Bayer 8x8
 * matrix, JBIG-encodes each plane using libjbig (jbigkit), and writes the
 * proprietary DDST stream to stdout.
 *
 * Stream layout (see DDST_FORMAT.md):
 *   GJET(168) GDIJ(120)  [GDIP(64) [GDIB(32) + K + Y + M + C]*]  JIDG(4)
 *
 * All multi-byte header fields are big-endian.
 *
 * Build: see Makefile. Requires libcups, libcupsimage, libjbig.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cups/cups.h>
#include <cups/raster.h>
#include <jbig.h>

/* --- DDST constants -------------------------------------------------- */

#define MAGIC_GJET "GJET"
#define MAGIC_GDIJ "GDIJ"
#define MAGIC_GDIP "GDIP"
#define MAGIC_GDIB "GDIB"
#define MAGIC_END  "JIDG"

#define GJET_LEN   0xA8u  /* 168 */
#define GDIJ_LEN   0x78u  /* 120 */
#define GDIP_LEN   0x40u  /*  64 */
#define GDIB_LEN   0x20u  /*  32 */

#define BAND_HEIGHT 256u  /* lines per band */

/* Media size codes (byte at GDIP+0x08). */
struct media_code {
    const char *name;
    uint8_t     code;
};

static const struct media_code MEDIA_TABLE[] = {
    {"Letter",     0x01},
    {"Legal",      0x05},
    {"Executive",  0x07},
    {"A3",         0x08},
    {"A4",         0x09},
    {"A5",         0x0B},
    {"A6",         0x46},
    {"B4",         0x0C},
    {"B5",         0x0D}, /* JIS B5 */
    {"JISB5",      0x0D},
    {"B6",         0x58}, /* JIS B6 */
    {"FS",         0x10},
    {"Folio",      0x0F},
    {"Foolscap",   0x0E},
    {"Env10",      0x14},
    {"Monarch",    0x25},
    {"EnvMonarch", 0x25},
    {"DL",         0x1B}, /* EnvDL */
    {"EnvDL",      0x1B},
    {"C5",         0x1C}, /* EnvC5 */
    {"EnvC5",      0x1C},
    {"C6",         0x1F}, /* EnvC6 */
    {"EnvC6",      0x1F},
    {"Ledger",     0x11},
    {"HalfLetter", 0x06},
    {"Kai8",       0x5C},
    {"Kai16",      0x5D},
    {"Postcard",   0x2B},
    {"ReplyPaid",  0x45},
    {"Custom",     0xFF},
    {NULL, 0},
};

static uint8_t media_code_for(const char *name)
{
    for (const struct media_code *m = MEDIA_TABLE; m->name; m++)
        if (strcasecmp(m->name, name) == 0)
            return m->code;
    return 0x09; /* A4 fallback (code 0x09) */
}

/* Map a page's point dimensions (from cupsPageSize) to a media size code,
 * matching either orientation within a small tolerance. Returns 0 when the
 * geometry does not match a known size (caller then uses the job option). */
static uint8_t media_code_from_points(unsigned w, unsigned h)
{
    static const struct { unsigned w, h; uint8_t code; } sizes[] = {
        {595,  842, 0x09}, /* A4 */
        {612,  792, 0x01}, /* Letter */
        {612, 1008, 0x05}, /* Legal */
        {420,  595, 0x0B}, /* A5 */
        {516,  729, 0x0D}, /* B5 (JIS) */
        {363,  516, 0x58}, /* B6 (JIS) */
        {298,  420, 0x46}, /* A6 */
        {522,  756, 0x07}, /* Executive */
        {297,  684, 0x14}, /* Env #10 */
        {279,  540, 0x25}, /* Env Monarch */
        {312,  624, 0x1B}, /* Env DL */
        {459,  649, 0x1C}, /* Env C5 */
        {323,  459, 0x1F}, /* Env C6 */
        {284,  419, 0x2B}, /* Postcard */
    };
    const unsigned tol = 3;  /* cupsPageSize is float; truncation loses ~1pt */
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned sw = sizes[i].w, sh = sizes[i].h;
        int portrait  = (w + tol >= sw && w <= sw + tol &&
                         h + tol >= sh && h <= sh + tol);
        int landscape = (w + tol >= sh && w <= sh + tol &&
                         h + tol >= sw && h <= sw + tol);
        if (portrait || landscape)
            return sizes[i].code;
    }
    return 0;
}

static uint8_t tray_code_for(const char *name)
{
    if (strcasecmp(name, "Automatic") == 0 || strcasecmp(name, "Auto") == 0)
        return 0x00;
    if (strcasecmp(name, "Manual") == 0 || strcasecmp(name, "Bypass") == 0)
        return 0x01;
    if (strcasecmp(name, "Tray1") == 0 || strcasecmp(name, "Tray 1") == 0 || strcasecmp(name, "1") == 0)
        return 0x02;
    if (strcasecmp(name, "Tray2") == 0 || strcasecmp(name, "Tray 2") == 0 || strcasecmp(name, "2") == 0)
        return 0x03;
    if (strcasecmp(name, "Tray3") == 0 || strcasecmp(name, "Tray 3") == 0 || strcasecmp(name, "3") == 0)
        return 0x04;
    if (strcasecmp(name, "Tray4") == 0 || strcasecmp(name, "Tray 4") == 0 || strcasecmp(name, "4") == 0)
        return 0x05;
    return 0x00; /* default automatic */
}

static uint8_t paper_type_code_for(const char *name)
{
    if (strcasecmp(name, "PlainRecycled") == 0) return 0x00;
    if (strcasecmp(name, "Plain") == 0 || strcasecmp(name, "Plain Paper") == 0 || strcasecmp(name, "Normal") == 0) return 0x01;
    if (strcasecmp(name, "Recycled") == 0 || strcasecmp(name, "Recycled Paper") == 0) return 0x02;
    if (strcasecmp(name, "Color") == 0 || strcasecmp(name, "Colour") == 0) return 0x03;
    if (strcasecmp(name, "Letterhead") == 0) return 0x04;
    if (strcasecmp(name, "Preprinted") == 0) return 0x05;
    if (strcasecmp(name, "Prepunched") == 0) return 0x06;
    if (strcasecmp(name, "Labels") == 0 || strcasecmp(name, "Label") == 0) return 0x07;
    if (strcasecmp(name, "Bond") == 0) return 0x08;
    if (strcasecmp(name, "Cardstock") == 0 || strcasecmp(name, "Card") == 0) return 0x09;
    if (strcasecmp(name, "Thick") == 0 || strcasecmp(name, "Thick Paper") == 0) return 0x0C;
    if (strcasecmp(name, "Thick160") == 0) return 0x0D;
    if (strcasecmp(name, "Envelope") == 0) return 0x0E;
    if (strcasecmp(name, "Thin") == 0 || strcasecmp(name, "Thin Paper") == 0) return 0x0F;
    if (strcasecmp(name, "Plain90") == 0) return 0x10;
    if (strcasecmp(name, "Thinner") == 0) return 0x12;
    return 0x01; /* default Plain */
}

static uint8_t cups_media_position_to_tray(unsigned int cupsMediaPosition)
{
    switch (cupsMediaPosition) {
        case 1: return 0x02; /* Tray 1 */
        case 2: return 0x03; /* Tray 2 */
        case 3: return 0x04; /* Tray 3 */
        case 4: return 0x01; /* Bypass / Manual */
        case 5: return 0x05; /* Tray 4 / Fallback */
        default: return 0x00; /* Auto */
    }
}

static uint8_t cups_media_type_to_paper_type(unsigned int cupsMediaType)
{
    switch (cupsMediaType) {
        case 0: return 0x01; /* Plain */
        case 1: return 0x0C; /* Thick */
        case 2: return 0x0F; /* Thin */
        case 3: return 0x02; /* Recycled */
        case 4: return 0x0E; /* Envelope */
        case 5: return 0x07; /* Labels */
        default: return 0x01; /* default Plain */
    }
}

/* --- Byte writers ---------------------------------------------------- */

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

/* --- Growable buffer for JBIG callback ------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} buf_t;

static void buf_init(buf_t *b)
{
    b->cap = 1 << 16;
    b->size = 0;
    b->data = malloc(b->cap);
    if (!b->data) {
        fputs("rastertoddst: out of memory\n", stderr);
        exit(1);
    }
}

static void buf_reset(buf_t *b)
{
    b->size = 0;
}

static void buf_free(buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->size = b->cap = 0;
}

static void buf_append(buf_t *b, const uint8_t *src, size_t n)
{
    if (b->size + n > b->cap) {
        size_t newcap = b->cap;
        while (newcap < b->size + n)
            newcap *= 2;
        b->data = realloc(b->data, newcap);
        if (!b->data) {
            fputs("rastertoddst: out of memory in buf_append\n", stderr);
            exit(1);
        }
        b->cap = newcap;
    }
    memcpy(b->data + b->size, src, n);
    b->size += n;
}

static void jbig_out_cb(unsigned char *start, size_t len, void *arg)
{
    buf_append((buf_t *)arg, start, len);
}

/* --- JBIG encode one packed-1bit MSB plane --------------------------- */

/* Encodes a w×h 1-bit MSB-first packed bitmap into a BIE. The output
 * is appended to *out. Encoder parameters: order=0x03, options=0x48
 * (LRLTWO | TPDON), L0=256. The printer's DDST/JBIG decoder expects the
 * BIH options byte to be 0x48; TPDON is a no-op on this single-layer
 * image, but the byte must still carry it. */
static void jbig_encode(buf_t *out, uint8_t *plane,
                        unsigned long w, unsigned long h)
{
    if (w == 0 || h == 0)
        return;
    struct jbg_enc_state enc;
    uint8_t *planes[1] = { plane };
    jbg_enc_init(&enc, w, h, 1, planes, jbig_out_cb, out);
    jbg_enc_options(&enc, 0x03, 0x48, BAND_HEIGHT, 0, 0);
    jbg_enc_out(&enc);
    jbg_enc_free(&enc);
}

/* --- Bayer 8×8 ordered dither, 8bpp coverage → 1bit MSB --------------- */

static const uint8_t BAYER8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};

/* Halftone an 8bpp coverage plane (rows of width 'src_width', stride
 * 'src_stride') into a packed-1bit MSB-first plane of width 'pad_width'
 * padded with zeros (= no ink). 'h' is the band height in scanlines.
 * The destination buffer must be (pad_width/8) * h bytes. */
static void halftone_plane(const uint8_t *src, unsigned src_width,
                           unsigned src_stride, unsigned h,
                           unsigned pad_width, unsigned y_offset,
                           uint8_t *dst)
{
    const unsigned dst_stride = pad_width / 8;
    memset(dst, 0, (size_t)dst_stride * h);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * src_stride;
        uint8_t *drow = dst + (size_t)y * dst_stride;
        const uint8_t *thr_row = BAYER8[(y + y_offset) & 7];
        for (unsigned x = 0; x < src_width; x++) {
            unsigned t = (unsigned)thr_row[x & 7] * 4 + 2;
            if (row[x] > t)
                drow[x >> 3] |= (uint8_t)(0x80u >> (x & 7));
        }
        /* Padding pixels [src_width..pad_width) stay 0 (no ink). */
    }
}

/* --- Plane separation ------------------------------------------------ */

/* Extract the i-th channel from a chunky 8bpp row into a contiguous
 * 8bpp single-channel plane. 'channels' is the chunky stride per pixel. */
static void extract_channel(const uint8_t *src, unsigned src_stride,
                            unsigned width, unsigned height,
                            unsigned channels, unsigned channel_index,
                            uint8_t *dst)
{
    for (unsigned y = 0; y < height; y++) {
        const uint8_t *row = src + (size_t)y * src_stride + channel_index;
        uint8_t *drow = dst + (size_t)y * width;
        for (unsigned x = 0; x < width; x++)
            drow[x] = row[x * channels];
    }
}

/* Convert chunky RGB (24bpp) to four contiguous 8bpp CMYK planes.
 * Uses textbook UCR: K = min(C,M,Y), then subtract. */
static void rgb_to_cmyk_planes(const uint8_t *src, unsigned src_stride,
                               unsigned w, unsigned h,
                               uint8_t *c, uint8_t *m,
                               uint8_t *y, uint8_t *k)
{
    for (unsigned yy = 0; yy < h; yy++) {
        const uint8_t *row = src + (size_t)yy * src_stride;
        for (unsigned xx = 0; xx < w; xx++) {
            uint8_t r_v = row[xx * 3 + 0];
            uint8_t g_v = row[xx * 3 + 1];
            uint8_t b_v = row[xx * 3 + 2];
            uint8_t c_v = (uint8_t)(255 - r_v);
            uint8_t m_v = (uint8_t)(255 - g_v);
            uint8_t y_v = (uint8_t)(255 - b_v);
            uint8_t k_v = c_v < m_v ? c_v : m_v;
            if (y_v < k_v) k_v = y_v;
            size_t idx = (size_t)yy * w + xx;
            c[idx] = (uint8_t)(c_v - k_v);
            m[idx] = (uint8_t)(m_v - k_v);
            y[idx] = (uint8_t)(y_v - k_v);
            k[idx] = k_v;
        }
    }
}

/* --- Per-plane worker for parallel halftone + JBIG -------------------- */

typedef struct {
    /* Input */
    const uint8_t *plane;       /* 8bpp coverage, src_stride per row */
    unsigned       src_width;
    unsigned       src_stride;
    unsigned       height;
    unsigned       pad_width;
    unsigned       y_offset;    /* absolute y of the band's first line */
    uint8_t        id;          /* 'K' / 'Y' / 'M' / 'C' for debug */
    /* Scratch + output (owned by main thread, reused per band) */
    uint8_t       *halftone;    /* band_byte_stride * BAND_HEIGHT */
    buf_t          out;
} plane_worker_t;

static void *plane_worker_fn(void *arg)
{
    plane_worker_t *w = arg;
    halftone_plane(w->plane, w->src_width, w->src_stride, w->height,
                   w->pad_width, w->y_offset, w->halftone);
    jbig_encode(&w->out, w->halftone, w->pad_width, w->height);
    return NULL;
}

static int threads_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("RASTERTODDST_THREADS");
        cached = (v && strcmp(v, "0") == 0) ? 0 : 1;
    }
    return cached;
}

static void run_plane_workers(plane_worker_t *workers, int count)
{
    if (count <= 1 || !threads_enabled()) {
        for (int i = 0; i < count; i++)
            plane_worker_fn(&workers[i]);
        return;
    }
    pthread_t tids[4];
    for (int i = 0; i < count; i++) {
        if (pthread_create(&tids[i], NULL, plane_worker_fn, &workers[i]) != 0) {
            /* Thread creation failure: run remaining synchronously to avoid
             * stalling the job. */
            for (int j = i; j < count; j++)
                plane_worker_fn(&workers[j]);
            for (int j = 0; j < i; j++)
                pthread_join(tids[j], NULL);
            return;
        }
    }
    for (int i = 0; i < count; i++)
        pthread_join(tids[i], NULL);
}

/* --- Page state ------------------------------------------------------ */

typedef struct {
    uint8_t *c;  /* 8bpp coverage plane, w*h bytes each */
    uint8_t *m;
    uint8_t *y;
    uint8_t *k;
    int      color; /* 1 if all 4 planes used, 0 if K only */
} planes_t;

static void planes_alloc(planes_t *p, unsigned w, unsigned h, int color)
{
    size_t n = (size_t)w * h;
    p->color = color;
    p->k = malloc(n);
    if (!p->k) goto oom;
    if (color) {
        p->c = malloc(n);
        p->m = malloc(n);
        p->y = malloc(n);
        if (!p->c || !p->m || !p->y) goto oom;
    } else {
        p->c = p->m = p->y = NULL;
    }
    return;
oom:
    fputs("rastertoddst: out of memory allocating planes\n", stderr);
    exit(1);
}

static void planes_free(planes_t *p)
{
    free(p->c); free(p->m); free(p->y); free(p->k);
    p->c = p->m = p->y = p->k = NULL;
}

/* --- Header writers -------------------------------------------------- */

static void write_gjet(FILE *out, const char *user, const char *title)
{
    uint8_t buf[GJET_LEN] = {0};
    memcpy(buf + 0x00, MAGIC_GJET, 4);
    put_be32(buf + 0x04, GJET_LEN);

    char host[64] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0)
        strncpy(host, "localhost", sizeof(host) - 1);
    memcpy(buf + 0x08, host, strnlen(host, 64));
    memcpy(buf + 0x48, title ? title : "Document",
           strnlen(title ? title : "Document", 64));
    memcpy(buf + 0x98, user ? user : "lp",
           strnlen(user ? user : "lp", 16));
    fwrite(buf, 1, GJET_LEN, out);
}

static void write_gdij(FILE *out, unsigned copies, int color,
                       int duplex, int collate)
{
    uint8_t buf[GDIJ_LEN] = {0};
    memcpy(buf + 0x00, MAGIC_GDIJ, 4);
    put_be32(buf + 0x04, GDIJ_LEN);
    buf[0x08] = 0x00;
    buf[0x09] = 0x64;  /* format version 100 */
    /* 0x0A: job copy count (big-endian u16), from the raster header's
     * NumCopies. This field is the number of copies, NOT the page
     * bit-height; writing the pixel height here put an out-of-range value
     * in the copy field. */
    put_be16(buf + 0x0A, (uint16_t)copies);
    /* 0x10 = duplex byte (0x00 simplex, 0x02 duplex);
     * 0x11 = collate byte (0x00 off, 0x08 on). The two are independent —
     * collate does not gate duplex. */
    buf[0x10] = duplex ? 0x02 : 0x00;
    buf[0x11] = collate ? 0x08 : 0x00;
    put_be16(buf + 0x12, 0x00A8);
    put_be32(buf + 0x14, (uint32_t)getpid());
    buf[0x18] = color ? 0x01 : 0x00;
    /* 0x20 = 0x01 (constant); 0x21 = ditherBPP-1 (0 for 1-bit halftone). */
    buf[0x20] = 0x01;
    buf[0x21] = 0x00;
    /* 0x22/0x23: constant 0x0200 stored little-endian -> bytes 0x00 0x02. */
    buf[0x22] = 0x00;
    buf[0x23] = 0x02;

    char host[64] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0)
        strncpy(host, "localhost", sizeof(host) - 1);
    memcpy(buf + 0x38, host, strnlen(host, 64));
    fwrite(buf, 1, GDIJ_LEN, out);
}

static void write_gdip(FILE *out, unsigned width, unsigned height,
                       int color, uint8_t media_code, uint8_t tray_code, uint8_t paper_type,
                       int duplex, unsigned page_index, unsigned band_count,
                       unsigned page_w_points, unsigned page_h_points)
{
    uint8_t buf[GDIP_LEN] = {0};
    memcpy(buf + 0x00, MAGIC_GDIP, 4);
    put_be32(buf + 0x04, GDIP_LEN);
    buf[0x08] = media_code;
    buf[0x09] = tray_code;
    buf[0x0A] = paper_type;
    /* buf[0x0B] is reserved (zero) */
    put_be16(buf + 0x0C, (uint16_t)width);
    put_be16(buf + 0x0E, (uint16_t)height);
    buf[0x20] = color ? 0x04 : 0x01;
    
    /* Face marker (0x21) and side index (0x22). cups_page is the 1-based CUPS
     * page number (page_index is 0-based, so +1).
     *   simplex: face 0x01, side = cups_page.
     *   duplex : face = (cups_page even) ? 0x05 : 0x0D;
     *            side = (cups_page odd) ? cups_page-1 : cups_page+1  (pair-swapped,
     *            e.g. pages 1,2,3,4 -> sides 0,3,2,5). */
    unsigned cups_page = page_index + 1;
    if (duplex) {
        buf[0x21] = (cups_page % 2 == 0) ? 0x05 : 0x0D;
        unsigned side = (cups_page & 1u) ? (cups_page - 1) : (cups_page + 1);
        put_be16(buf + 0x22, (uint16_t)side);
    } else {
        buf[0x21] = 0x01;
        put_be16(buf + 0x22, (uint16_t)cups_page);
    }
    
    put_be16(buf + 0x36, (uint16_t)band_count);
    put_be32(buf + 0x38, page_w_points);
    put_be32(buf + 0x3C, page_h_points);
    fwrite(buf, 1, GDIP_LEN, out);
}

static void write_gdib(FILE *out,
                       uint32_t k_size, uint32_t y_size,
                       uint32_t m_size, uint32_t c_size,
                       unsigned band_w, unsigned band_h,
                       int first_band, int last_band, int last_of_page)
{
    uint8_t buf[GDIB_LEN] = {0};
    memcpy(buf + 0x00, MAGIC_GDIB, 4);
    uint32_t flags = 0;
    if (first_band) flags |= 1;
    if (last_band)  flags |= 2;
    put_be32(buf + 0x04, flags);
    put_be32(buf + 0x08, k_size);
    put_be32(buf + 0x0C, y_size);
    put_be32(buf + 0x10, m_size);
    put_be32(buf + 0x14, c_size);
    put_be16(buf + 0x18, (uint16_t)band_w);
    put_be16(buf + 0x1A, (uint16_t)band_h);
    buf[0x1E] = last_of_page ? 1 : 0;
    fwrite(buf, 1, GDIB_LEN, out);
}

/* --- Options parsing ------------------------------------------------- */

static char *opt_value(const char *options, const char *key, char *out, size_t n)
{
    /* Match "key=value" or "key" tokens. Boolean keys (no '=') return "true". */
    size_t klen = strlen(key);
    const char *p = options;
    while (*p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t tlen = (size_t)(p - start);
        if (tlen == klen && strncmp(start, key, klen) == 0) {
            strncpy(out, "true", n - 1);
            out[n - 1] = 0;
            return out;
        }
        if (tlen > klen + 1 &&
            strncmp(start, key, klen) == 0 &&
            start[klen] == '=') {
            size_t vlen = tlen - klen - 1;
            if (vlen >= n) vlen = n - 1;
            memcpy(out, start + klen + 1, vlen);
            out[vlen] = 0;
            return out;
        }
    }
    out[0] = 0;
    return out;
}

/* --- Main ------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 6) {
        fputs("Usage: rastertoddst job-id user title copies options [filename]\n", stderr);
        return 1;
    }
    const char *user  = argv[2];
    const char *title = argv[3];
    const char *opts  = argv[5];

    /* Open input: file argv[6] if provided, else stdin. */
    int infd = 0;
    if (argc >= 7) {
        infd = open(argv[6], O_RDONLY);
        if (infd < 0) {
            fprintf(stderr, "rastertoddst: cannot open %s: %s\n",
                    argv[6], strerror(errno));
            return 1;
        }
    }

    cups_raster_t *ras = cupsRasterOpen(infd, CUPS_RASTER_READ);
    if (!ras) {
        fputs("rastertoddst: cupsRasterOpen failed\n", stderr);
        return 1;
    }

    char media_name[64];
    opt_value(opts, "PageSize", media_name, sizeof(media_name));
    if (!media_name[0]) opt_value(opts, "media", media_name, sizeof(media_name));
    if (!media_name[0]) strcpy(media_name, "A4");
    uint8_t media_opt = media_code_for(media_name);  /* job-level fallback */

    char tray_name[64];
    opt_value(opts, "InputSlot", tray_name, sizeof(tray_name));
    if (!tray_name[0]) opt_value(opts, "InputTray", tray_name, sizeof(tray_name));

    char type_name[64];
    opt_value(opts, "MediaType", type_name, sizeof(type_name));
    if (!type_name[0]) opt_value(opts, "PaperType", type_name, sizeof(type_name));

    char duplex_s[16];
    opt_value(opts, "Duplex", duplex_s, sizeof(duplex_s));
    int duplex = duplex_s[0] && strcasecmp(duplex_s, "None") != 0 &&
                 strcasecmp(duplex_s, "false") != 0;

    char collate_s[16];
    opt_value(opts, "Collate", collate_s, sizeof(collate_s));
    int collate = (strcasecmp(collate_s, "true") == 0);

    char colormodel[16];
    opt_value(opts, "ColorModel", colormodel, sizeof(colormodel));
    int force_mono = (strcasecmp(colormodel, "Gray") == 0) ||
                     (strcasecmp(colormodel, "Mono") == 0) ||
                     (strcasecmp(colormodel, "Black") == 0);

    int sent_job_headers = 0;
    unsigned page_index = 0;

    cups_page_header2_t hdr;
    while (cupsRasterReadHeader2(ras, &hdr)) {
        unsigned w = hdr.cupsWidth;
        unsigned h = hdr.cupsHeight;
        unsigned bpp = hdr.cupsBitsPerPixel;
        unsigned bpl = hdr.cupsBytesPerLine;
        unsigned cs  = hdr.cupsColorSpace;

        int color = !force_mono &&
                    (cs == CUPS_CSPACE_RGB || cs == CUPS_CSPACE_RGBA ||
                     cs == CUPS_CSPACE_CMYK || cs == CUPS_CSPACE_SRGB);

        /* Read the whole raster page into memory. */
        size_t raster_bytes = (size_t)bpl * h;
        uint8_t *raster = malloc(raster_bytes);
        if (!raster) {
            fputs("rastertoddst: out of memory for raster\n", stderr);
            return 1;
        }
        if (cupsRasterReadPixels(ras, raster, raster_bytes) != raster_bytes) {
            fputs("rastertoddst: short raster read\n", stderr);
            return 1;
        }

        /* Separate into 8bpp coverage planes. */
        planes_t planes;
        planes_alloc(&planes, w, h, color);

        if (color && cs == CUPS_CSPACE_CMYK && bpp == 32) {
            extract_channel(raster, bpl, w, h, 4, 0, planes.c);
            extract_channel(raster, bpl, w, h, 4, 1, planes.m);
            extract_channel(raster, bpl, w, h, 4, 2, planes.y);
            extract_channel(raster, bpl, w, h, 4, 3, planes.k);
        } else if (color && (cs == CUPS_CSPACE_RGB || cs == CUPS_CSPACE_SRGB) && bpp == 24) {
            rgb_to_cmyk_planes(raster, bpl, w, h,
                               planes.c, planes.m, planes.y, planes.k);
        } else if (cs == CUPS_CSPACE_K && bpp == 8) {
            extract_channel(raster, bpl, w, h, 1, 0, planes.k);
        } else if (cs == CUPS_CSPACE_W && bpp == 8) {
            /* CUPS_CSPACE_W: 0 = black, 255 = white. Invert to coverage. */
            for (size_t i = 0; i < (size_t)w * h; i++) {
                size_t y = i / w, x = i % w;
                planes.k[i] = (uint8_t)(255u - raster[y * bpl + x]);
            }
        } else if (color && cs == CUPS_CSPACE_RGBA && bpp == 32) {
            /* Drop alpha then UCR. */
            uint8_t *rgb = malloc((size_t)w * h * 3);
            if (!rgb) { fputs("oom rgb\n", stderr); return 1; }
            for (unsigned yy = 0; yy < h; yy++) {
                const uint8_t *row = raster + (size_t)yy * bpl;
                uint8_t *dr = rgb + (size_t)yy * w * 3;
                for (unsigned xx = 0; xx < w; xx++) {
                    dr[xx * 3 + 0] = row[xx * 4 + 0];
                    dr[xx * 3 + 1] = row[xx * 4 + 1];
                    dr[xx * 3 + 2] = row[xx * 4 + 2];
                }
            }
            rgb_to_cmyk_planes(rgb, w * 3, w, h,
                               planes.c, planes.m, planes.y, planes.k);
            free(rgb);
        } else {
            fprintf(stderr,
                    "rastertoddst: unsupported color space %u (bpp=%u)\n",
                    cs, bpp);
            return 1;
        }
        free(raster);

        /* Pad width to multiple of 32 pixels (4 bytes) for JBIG alignment. */
        unsigned pad_w = (w + 31u) & ~31u;

        /* Emit headers on first page. */
        if (!sent_job_headers) {
            write_gjet(stdout, user, title);
            write_gdij(stdout, hdr.NumCopies ? hdr.NumCopies : 1,
                       color, duplex, collate);
            sent_job_headers = 1;
        }

        unsigned band_count = (h + BAND_HEIGHT - 1) / BAND_HEIGHT;
        unsigned page_w_points = (unsigned)hdr.cupsPageSize[0];
        unsigned page_h_points = (unsigned)hdr.cupsPageSize[1];
        /* Prefer the media code matching this page's actual geometry so a
         * mixed-size job feeds the right size on each page; fall back to the
         * job-level PageSize option when the geometry is unrecognised. */
        uint8_t media_pg = media_code_from_points(page_w_points, page_h_points);
        uint8_t media = media_pg ? media_pg : media_opt;
        uint8_t tray_code = tray_name[0] ? tray_code_for(tray_name) : cups_media_position_to_tray(hdr.MediaPosition);
        uint8_t paper_type = type_name[0] ? paper_type_code_for(type_name) : cups_media_type_to_paper_type(hdr.cupsMediaType);

        write_gdip(stdout, pad_w, h, color, media, tray_code, paper_type,
                   duplex, page_index, band_count,
                   page_w_points, page_h_points);

        /* Encode each band. Four CMYK planes are independent so we
         * halftone + JBIG-encode them in parallel using one thread
         * per plane (one for mono). Each worker owns its own halftone
         * scratch buffer and JBIG output buf to avoid contention. */
        const size_t band_byte_stride = pad_w / 8u;

        plane_worker_t workers[4];
        const int worker_count = color ? 4 : 1;
        uint8_t plane_id[4] = {'K', 'Y', 'M', 'C'};
        uint8_t *plane_src[4] = {planes.k, planes.y, planes.m, planes.c};
        for (int i = 0; i < worker_count; i++) {
            workers[i].halftone = malloc(band_byte_stride * BAND_HEIGHT);
            if (!workers[i].halftone) {
                fputs("rastertoddst: oom halftone\n", stderr);
                return 1;
            }
            buf_init(&workers[i].out);
            workers[i].id = plane_id[i];
        }

        for (unsigned band = 0; band < band_count; band++) {
            unsigned y0 = band * BAND_HEIGHT;
            unsigned y1 = y0 + BAND_HEIGHT;
            if (y1 > h) y1 = h;
            unsigned bh = y1 - y0;

            /* Set up workers for this band and dispatch. */
            for (int i = 0; i < worker_count; i++) {
                workers[i].plane = plane_src[i] + (size_t)y0 * w;
                workers[i].src_width  = w;
                workers[i].src_stride = w;
                workers[i].height = bh;
                workers[i].pad_width = pad_w;
                workers[i].y_offset = y0;
                buf_reset(&workers[i].out);
            }
            run_plane_workers(workers, worker_count);

            /* Workers are indexed K=0, Y=1, M=2, C=3. */
            int first = (band == 0);
            int last  = (band == band_count - 1);
            write_gdib(stdout,
                       (uint32_t)workers[0].out.size,
                       color ? (uint32_t)workers[1].out.size : 0,
                       color ? (uint32_t)workers[2].out.size : 0,
                       color ? (uint32_t)workers[3].out.size : 0,
                       pad_w, bh, first, last, last);
            fwrite(workers[0].out.data, 1, workers[0].out.size, stdout);
            if (color) {
                fwrite(workers[1].out.data, 1, workers[1].out.size, stdout);
                fwrite(workers[2].out.data, 1, workers[2].out.size, stdout);
                fwrite(workers[3].out.data, 1, workers[3].out.size, stdout);
            }
        }

        for (int i = 0; i < worker_count; i++) {
            free(workers[i].halftone);
            buf_free(&workers[i].out);
        }
        planes_free(&planes);

        fprintf(stderr, "PAGE: %u 1\n", page_index + 1);
        page_index++;
    }

    if (sent_job_headers)
        fwrite(MAGIC_END, 1, 4, stdout);

    cupsRasterClose(ras);
    if (infd != 0) close(infd);
    return 0;
}
