/*
 * sane-ricoh-spc240sf — SANE backend for the Ricoh Aficio SP C240SF MFP.
 *
 * The SP C240SF predates AirScan/eSCL, so this backend tries two
 * scan transports in order:
 *
 *   1. eSCL  — in case a recent firmware ships it (none observed, but
 *              the probe is cheap and protects future firmware updates).
 *   2. Ricoh Network TWAIN  — HTTP/SOAP on port 80 + a binary side-
 *              channel.  The wire format is proprietary and undocumented;
 *              the functions in the SCAN PROTOCOL block below are stubs
 *              that need to be completed from a USB or network capture.
 *
 * Until the proprietary protocol is filled in, scans will fail with
 * SANE_STATUS_UNSUPPORTED, but the backend will still register the
 * device, expose options, and let `scanimage -L` and the GUIs (xsane,
 * simple-scan) see it for diagnostics.
 *
 * Build:
 *   make sane    (produces libsane-ricoh-spc240sf.so)
 *
 * Install:
 *   sudo make install-sane    (also drops a stanza into /etc/sane.d/)
 *
 * Configure /etc/sane.d/ricoh-spc240sf.conf with the printer's IP.
 */

#define BACKEND_NAME ricoh_spc240sf

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <curl/curl.h>

#include <sane/sane.h>
#include <sane/saneopts.h>

/* ----- Backend identification --------------------------------------- */

#define BACKEND_BUILD 1
#define VENDOR "Ricoh"
#define MODEL  "Aficio SP C240SF"

/* ----- Device state -------------------------------------------------- */

typedef enum {
    XPORT_UNKNOWN = 0,
    XPORT_ESCL    = 1,
    XPORT_RICOH   = 2,
} transport_t;

typedef struct device {
    struct device *next;
    char           name[64];   /* SANE device name; e.g. "ricoh-spc240sf:0" */
    char           url[128];   /* http://<ip>/  */
    transport_t    transport;
    SANE_Device    sane;       /* exposed through sane_get_devices */
} device_t;

typedef struct handle {
    device_t      *dev;
    int            scanning;
    /* selected option values */
    int            resolution;
    int            mode;       /* 0 = color, 1 = gray, 2 = lineart */
    int            source;     /* 0 = flatbed, 1 = ADF, 2 = ADF duplex */
    int            paper_size; /* 0 = A4, 1 = Letter, ... */
    SANE_Parameters params;
    /* image buffer (filled by transport-specific scan start) */
    uint8_t       *image;
    size_t         image_size;
    size_t         image_pos;
} handle_t;

static device_t *g_devices = NULL;
static char      g_conf_paths[8][128];
static int       g_conf_count = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ----- Logging ------------------------------------------------------- */

static void blog(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void blog(const char *fmt, ...)
{
    if (!getenv("SANE_DEBUG_RICOH_SPC240SF")) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[ricoh-spc240sf] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#include <stdarg.h>  /* needed by blog */

/* ----- HTTP helpers -------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} http_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    http_buf_t *b = userdata;
    size_t n = size * nmemb;
    if (b->size + n > b->cap) {
        size_t newcap = b->cap ? b->cap : 4096;
        while (newcap < b->size + n) newcap *= 2;
        uint8_t *nd = realloc(b->data, newcap);
        if (!nd) return 0;
        b->data = nd;
        b->cap = newcap;
    }
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    return n;
}

static int http_get(const char *url, http_buf_t *out, long timeout_ms)
{
    CURL *c = curl_easy_init();
    if (!c) return -1;
    out->size = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    return (rc == CURLE_OK) ? (int)status : -1;
}

/* ----- Discovery ----------------------------------------------------- */

/* Read host lines (one IP/hostname per line, '#' for comments) from the
 * SANE config file ${SANE_CONFIG_DIR:-/etc/sane.d}/ricoh-spc240sf.conf. */
static int read_config(void)
{
    const char *dir = getenv("SANE_CONFIG_DIR");
    if (!dir) dir = "/etc/sane.d";
    char path[256];
    snprintf(path, sizeof(path), "%s/ricoh-spc240sf.conf", dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[128];
    while (g_conf_count < 8 && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == 0) continue;
        size_t n = strlen(p);
        while (n && (p[n-1] == '\n' || p[n-1] == '\r' || p[n-1] == ' ')) p[--n] = 0;
        if (n == 0) continue;
        strncpy(g_conf_paths[g_conf_count], p,
                sizeof(g_conf_paths[0]) - 1);
        g_conf_paths[g_conf_count][sizeof(g_conf_paths[0]) - 1] = 0;
        g_conf_count++;
    }
    fclose(f);
    return g_conf_count;
}

/* Probe a single host. Returns the transport_t that responded, or
 * XPORT_UNKNOWN if nothing identifiable answered. */
static transport_t probe_host(const char *host)
{
    char url[160];
    http_buf_t buf = {0};

    /* 1. eSCL: GET /eSCL/ScannerCapabilities */
    snprintf(url, sizeof(url), "http://%s/eSCL/ScannerCapabilities", host);
    int status = http_get(url, &buf, 1500);
    if (status == 200 && buf.size > 0) {
        blog("probe %s: eSCL responded (%zu bytes)", host, buf.size);
        free(buf.data);
        return XPORT_ESCL;
    }
    free(buf.data); buf = (http_buf_t){0};

    /* 2. Ricoh WIM: GET / to confirm the device is a Ricoh, then
     *    GET /web/info.cgi or /web/guest/en/info.cgi for model name. */
    snprintf(url, sizeof(url), "http://%s/", host);
    status = http_get(url, &buf, 1500);
    if (status == 200 && buf.data &&
        (memmem(buf.data, buf.size, "RICOH", 5) ||
         memmem(buf.data, buf.size, "Ricoh", 5))) {
        blog("probe %s: Ricoh WIM detected", host);
        free(buf.data);
        return XPORT_RICOH;
    }
    free(buf.data);
    return XPORT_UNKNOWN;
}

static void add_device(const char *host, transport_t t)
{
    device_t *d = calloc(1, sizeof(*d));
    if (!d) return;
    snprintf(d->name, sizeof(d->name), "ricoh-spc240sf:%s", host);
    snprintf(d->url, sizeof(d->url), "http://%s", host);
    d->transport = t;
    d->sane.name   = d->name;
    d->sane.vendor = VENDOR;
    d->sane.model  = MODEL;
    d->sane.type   = "multi-function peripheral";
    d->next = g_devices;
    g_devices = d;
}

/* ----- SANE option machinery ---------------------------------------- */

enum {
    OPT_NUM_OPTS = 0,
    OPT_MODE_GROUP,
    OPT_MODE,
    OPT_RESOLUTION,
    OPT_SOURCE,
    OPT_GEOMETRY_GROUP,
    OPT_PAPER_SIZE,
    OPT_NUM_OPTIONS
};

static const SANE_String_Const mode_list[]  = { "Color", "Gray", "Lineart", NULL };
static const SANE_String_Const source_list[] = { "Flatbed", "ADF", "ADF Duplex", NULL };
static const SANE_String_Const paper_list[]  = { "A4", "Letter", "Legal", "A5", NULL };
static const SANE_Int          res_list[]    = { 5 /* count */, 75, 150, 200, 300, 600 };

static SANE_Option_Descriptor opts[OPT_NUM_OPTIONS];

static void init_options(handle_t *h)
{
    memset(opts, 0, sizeof(opts));

    opts[OPT_NUM_OPTS].name  = SANE_NAME_NUM_OPTIONS;
    opts[OPT_NUM_OPTS].title = SANE_TITLE_NUM_OPTIONS;
    opts[OPT_NUM_OPTS].desc  = SANE_DESC_NUM_OPTIONS;
    opts[OPT_NUM_OPTS].type  = SANE_TYPE_INT;
    opts[OPT_NUM_OPTS].size  = sizeof(SANE_Word);
    opts[OPT_NUM_OPTS].cap   = SANE_CAP_SOFT_DETECT;

    opts[OPT_MODE_GROUP].name  = "";
    opts[OPT_MODE_GROUP].title = "Scan mode";
    opts[OPT_MODE_GROUP].desc  = "";
    opts[OPT_MODE_GROUP].type  = SANE_TYPE_GROUP;

    opts[OPT_MODE].name  = SANE_NAME_SCAN_MODE;
    opts[OPT_MODE].title = SANE_TITLE_SCAN_MODE;
    opts[OPT_MODE].desc  = SANE_DESC_SCAN_MODE;
    opts[OPT_MODE].type  = SANE_TYPE_STRING;
    opts[OPT_MODE].size  = 16;
    opts[OPT_MODE].constraint_type = SANE_CONSTRAINT_STRING_LIST;
    opts[OPT_MODE].constraint.string_list = mode_list;
    opts[OPT_MODE].cap = SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT;

    opts[OPT_RESOLUTION].name  = SANE_NAME_SCAN_RESOLUTION;
    opts[OPT_RESOLUTION].title = SANE_TITLE_SCAN_RESOLUTION;
    opts[OPT_RESOLUTION].desc  = SANE_DESC_SCAN_RESOLUTION;
    opts[OPT_RESOLUTION].type  = SANE_TYPE_INT;
    opts[OPT_RESOLUTION].unit  = SANE_UNIT_DPI;
    opts[OPT_RESOLUTION].size  = sizeof(SANE_Word);
    opts[OPT_RESOLUTION].constraint_type = SANE_CONSTRAINT_WORD_LIST;
    opts[OPT_RESOLUTION].constraint.word_list = res_list;
    opts[OPT_RESOLUTION].cap = SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT;

    opts[OPT_SOURCE].name  = SANE_NAME_SCAN_SOURCE;
    opts[OPT_SOURCE].title = SANE_TITLE_SCAN_SOURCE;
    opts[OPT_SOURCE].desc  = SANE_DESC_SCAN_SOURCE;
    opts[OPT_SOURCE].type  = SANE_TYPE_STRING;
    opts[OPT_SOURCE].size  = 16;
    opts[OPT_SOURCE].constraint_type = SANE_CONSTRAINT_STRING_LIST;
    opts[OPT_SOURCE].constraint.string_list = source_list;
    opts[OPT_SOURCE].cap = SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT;

    opts[OPT_GEOMETRY_GROUP].name  = "";
    opts[OPT_GEOMETRY_GROUP].title = "Geometry";
    opts[OPT_GEOMETRY_GROUP].desc  = "";
    opts[OPT_GEOMETRY_GROUP].type  = SANE_TYPE_GROUP;

    opts[OPT_PAPER_SIZE].name  = "paper-size";
    opts[OPT_PAPER_SIZE].title = "Paper size";
    opts[OPT_PAPER_SIZE].desc  = "Paper size of the scanned originals.";
    opts[OPT_PAPER_SIZE].type  = SANE_TYPE_STRING;
    opts[OPT_PAPER_SIZE].size  = 16;
    opts[OPT_PAPER_SIZE].constraint_type = SANE_CONSTRAINT_STRING_LIST;
    opts[OPT_PAPER_SIZE].constraint.string_list = paper_list;
    opts[OPT_PAPER_SIZE].cap = SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT;

    h->resolution = 300;
    h->mode = 0;
    h->source = 0;
    h->paper_size = 0;
}

/* =========================================================================
 * SCAN PROTOCOL — to be filled in from a real capture.
 *
 * scan_start() opens a session with the printer, sends the parameters
 * (resolution, mode, paper size, source), tells it to begin scanning, and
 * downloads the page into h->image as a contiguous byte buffer.
 *
 * The SANE_Parameters in h->params describe what we tell the host about
 * h->image: pixels_per_line, lines, bytes_per_line, depth, format.
 *
 * Until a capture is available, this returns SANE_STATUS_UNSUPPORTED so
 * scanimage prints a clear error rather than producing a blank file.
 * ====================================================================== */

static SANE_Status scan_start_escl(handle_t *h)
{
    /* TODO: build an eSCL ScanSettings XML, POST to /eSCL/ScanJobs,
     *       follow the Location header, GET pages until 410 GONE. */
    (void)h;
    return SANE_STATUS_UNSUPPORTED;
}

static SANE_Status scan_start_ricoh(handle_t *h)
{
    /* TODO: implement the Ricoh proprietary HTTP/binary protocol once
     *       a successful Windows TWAIN capture is decoded. The shape
     *       expected is:
     *           1. POST /scancontrol with a "BeginScan" SOAP envelope
     *              containing the params we have in h.
     *           2. Read the returned session id.
     *           3. GET /scanstream?sid=... until Content-Length bytes
     *              are received; image data is JFIF for color or raw
     *              PGM for mono.
     *           4. POST /scancontrol with "EndScan".
     */
    (void)h;
    return SANE_STATUS_UNSUPPORTED;
}

/* ----- SANE entry points -------------------------------------------- */

SANE_Status sane_init(SANE_Int *version_code, SANE_Auth_Callback auth)
{
    (void)auth;
    if (version_code)
        *version_code = SANE_VERSION_CODE(SANE_CURRENT_MAJOR, SANE_CURRENT_MINOR, BACKEND_BUILD);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    read_config();
    return SANE_STATUS_GOOD;
}

void sane_exit(void)
{
    pthread_mutex_lock(&g_mtx);
    device_t *d = g_devices;
    while (d) { device_t *n = d->next; free(d); d = n; }
    g_devices = NULL;
    pthread_mutex_unlock(&g_mtx);
    curl_global_cleanup();
}

SANE_Status sane_get_devices(const SANE_Device ***device_list,
                             SANE_Bool local_only)
{
    (void)local_only;
    pthread_mutex_lock(&g_mtx);
    /* (Re-)probe every configured host. */
    device_t *old = g_devices;
    g_devices = NULL;
    while (old) { device_t *n = old->next; free(old); old = n; }
    for (int i = 0; i < g_conf_count; i++) {
        transport_t t = probe_host(g_conf_paths[i]);
        if (t != XPORT_UNKNOWN)
            add_device(g_conf_paths[i], t);
    }
    int n = 0;
    for (device_t *d = g_devices; d; d = d->next) n++;
    static const SANE_Device **list = NULL;
    free(list);
    list = calloc(n + 1, sizeof(*list));
    int i = 0;
    for (device_t *d = g_devices; d; d = d->next)
        list[i++] = &d->sane;
    list[i] = NULL;
    *device_list = list;
    pthread_mutex_unlock(&g_mtx);
    return SANE_STATUS_GOOD;
}

SANE_Status sane_open(SANE_String_Const name, SANE_Handle *handle)
{
    pthread_mutex_lock(&g_mtx);
    device_t *d = NULL;
    if (!name || !*name) {
        d = g_devices;
    } else {
        for (d = g_devices; d; d = d->next)
            if (strcmp(d->name, name) == 0)
                break;
    }
    pthread_mutex_unlock(&g_mtx);
    if (!d) return SANE_STATUS_INVAL;

    handle_t *h = calloc(1, sizeof(*h));
    if (!h) return SANE_STATUS_NO_MEM;
    h->dev = d;
    init_options(h);
    *handle = h;
    return SANE_STATUS_GOOD;
}

void sane_close(SANE_Handle handle)
{
    handle_t *h = handle;
    if (!h) return;
    free(h->image);
    free(h);
}

const SANE_Option_Descriptor *sane_get_option_descriptor(SANE_Handle handle, SANE_Int option)
{
    (void)handle;
    if (option < 0 || option >= OPT_NUM_OPTIONS) return NULL;
    if (option == OPT_NUM_OPTS) {
        static SANE_Word num = OPT_NUM_OPTIONS;
        (void)num;
    }
    return &opts[option];
}

SANE_Status sane_control_option(SANE_Handle handle, SANE_Int option,
                                SANE_Action action, void *value, SANE_Int *info)
{
    handle_t *h = handle;
    if (!h) return SANE_STATUS_INVAL;
    if (info) *info = 0;
    if (option < 0 || option >= OPT_NUM_OPTIONS) return SANE_STATUS_INVAL;

    switch (option) {
    case OPT_NUM_OPTS:
        if (action == SANE_ACTION_GET_VALUE)
            *(SANE_Word *)value = OPT_NUM_OPTIONS;
        return SANE_STATUS_GOOD;
    case OPT_RESOLUTION:
        if (action == SANE_ACTION_GET_VALUE) {
            *(SANE_Word *)value = h->resolution;
        } else if (action == SANE_ACTION_SET_VALUE) {
            h->resolution = *(const SANE_Word *)value;
            if (info) *info = SANE_INFO_RELOAD_PARAMS;
        }
        return SANE_STATUS_GOOD;
    case OPT_MODE: {
        const char *cur = mode_list[h->mode];
        if (action == SANE_ACTION_GET_VALUE) {
            strncpy(value, cur, 15); ((char*)value)[15] = 0;
        } else if (action == SANE_ACTION_SET_VALUE) {
            for (int i = 0; mode_list[i]; i++)
                if (strcasecmp(mode_list[i], (const char*)value) == 0) {
                    h->mode = i;
                    if (info) *info = SANE_INFO_RELOAD_PARAMS;
                    return SANE_STATUS_GOOD;
                }
            return SANE_STATUS_INVAL;
        }
        return SANE_STATUS_GOOD;
    }
    case OPT_SOURCE: {
        if (action == SANE_ACTION_GET_VALUE) {
            strncpy(value, source_list[h->source], 15);
            ((char*)value)[15] = 0;
        } else if (action == SANE_ACTION_SET_VALUE) {
            for (int i = 0; source_list[i]; i++)
                if (strcasecmp(source_list[i], (const char*)value) == 0) {
                    h->source = i;
                    return SANE_STATUS_GOOD;
                }
            return SANE_STATUS_INVAL;
        }
        return SANE_STATUS_GOOD;
    }
    case OPT_PAPER_SIZE: {
        if (action == SANE_ACTION_GET_VALUE) {
            strncpy(value, paper_list[h->paper_size], 15);
            ((char*)value)[15] = 0;
        } else if (action == SANE_ACTION_SET_VALUE) {
            for (int i = 0; paper_list[i]; i++)
                if (strcasecmp(paper_list[i], (const char*)value) == 0) {
                    h->paper_size = i;
                    if (info) *info = SANE_INFO_RELOAD_PARAMS;
                    return SANE_STATUS_GOOD;
                }
            return SANE_STATUS_INVAL;
        }
        return SANE_STATUS_GOOD;
    }
    case OPT_MODE_GROUP:
    case OPT_GEOMETRY_GROUP:
        return SANE_STATUS_INVAL;
    }
    return SANE_STATUS_INVAL;
}

static void compute_parameters(handle_t *h)
{
    /* Paper dimensions in mm for the modes we offer. */
    static const struct { int w_mm, h_mm; } sizes[] = {
        {210, 297}, /* A4 */
        {216, 279}, /* Letter */
        {216, 356}, /* Legal */
        {148, 210}, /* A5 */
    };
    int idx = h->paper_size;
    int w_mm = sizes[idx].w_mm;
    int h_mm = sizes[idx].h_mm;
    int dpi = h->resolution;
    int pixels = (w_mm * dpi + 12) / 25;     /* mm → in × dpi rounded */
    int lines  = (h_mm * dpi + 12) / 25;
    h->params.last_frame = SANE_TRUE;
    h->params.lines = lines;
    h->params.pixels_per_line = pixels;
    if (h->mode == 0) {
        h->params.format = SANE_FRAME_RGB;
        h->params.depth = 8;
        h->params.bytes_per_line = pixels * 3;
    } else if (h->mode == 1) {
        h->params.format = SANE_FRAME_GRAY;
        h->params.depth = 8;
        h->params.bytes_per_line = pixels;
    } else {
        h->params.format = SANE_FRAME_GRAY;
        h->params.depth = 1;
        h->params.bytes_per_line = (pixels + 7) / 8;
    }
}

SANE_Status sane_get_parameters(SANE_Handle handle, SANE_Parameters *params)
{
    handle_t *h = handle;
    if (!h) return SANE_STATUS_INVAL;
    compute_parameters(h);
    *params = h->params;
    return SANE_STATUS_GOOD;
}

SANE_Status sane_start(SANE_Handle handle)
{
    handle_t *h = handle;
    if (!h) return SANE_STATUS_INVAL;
    if (h->scanning) return SANE_STATUS_DEVICE_BUSY;
    compute_parameters(h);
    free(h->image); h->image = NULL;
    h->image_size = 0;
    h->image_pos = 0;
    h->scanning = 1;

    SANE_Status rc;
    if (h->dev->transport == XPORT_ESCL)
        rc = scan_start_escl(h);
    else
        rc = scan_start_ricoh(h);
    if (rc != SANE_STATUS_GOOD) {
        h->scanning = 0;
        return rc;
    }
    return SANE_STATUS_GOOD;
}

SANE_Status sane_read(SANE_Handle handle, SANE_Byte *data,
                      SANE_Int max_length, SANE_Int *length)
{
    handle_t *h = handle;
    *length = 0;
    if (!h || !h->scanning) return SANE_STATUS_INVAL;
    size_t remaining = h->image_size - h->image_pos;
    if (remaining == 0) {
        h->scanning = 0;
        return SANE_STATUS_EOF;
    }
    size_t n = (size_t)max_length < remaining ? (size_t)max_length : remaining;
    memcpy(data, h->image + h->image_pos, n);
    h->image_pos += n;
    *length = (SANE_Int)n;
    return SANE_STATUS_GOOD;
}

void sane_cancel(SANE_Handle handle)
{
    handle_t *h = handle;
    if (!h) return;
    h->scanning = 0;
}

SANE_Status sane_set_io_mode(SANE_Handle handle, SANE_Bool non_blocking)
{
    (void)handle;
    return non_blocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

SANE_Status sane_get_select_fd(SANE_Handle handle, SANE_Int *fd)
{
    (void)handle; (void)fd;
    return SANE_STATUS_UNSUPPORTED;
}
