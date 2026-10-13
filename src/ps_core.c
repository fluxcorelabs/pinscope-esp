/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 Filip Code. See LICENSE: for use with the Pinscope service only. */
/*
 * Pinscope core: encrypted connection to the service, stream multiplexer and
 * outbox. Portable C, single task. All buffers are static and fixed in size;
 * nothing about memory is configurable.
 *
 * See docs/PROTOCOL.md for the wire format.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pinscope.h"
#include "ps_crypto.h"
#include "ps_port.h"

/* ---- build-time defaults (Kconfig on ESP-IDF, plain macros elsewhere) ---- */

/* The Pinscope service: psl://host:port/<its public key, hex>. Not a setting:
 * the component is licensed for use with this service only (see LICENSE). */
#ifndef PINSCOPE_SERVICE_URL
#define PINSCOPE_SERVICE_URL \
    "psl://connect.pinscope.dev:443/71d9a78cbfd44a3f4debcf5f2c8df68c3b1f06276db365a292946df7b1e27421"
#endif
#ifndef CONFIG_PINSCOPE_PROJECT_KEY
#define CONFIG_PINSCOPE_PROJECT_KEY ""
#endif
#ifndef CONFIG_PINSCOPE_DEVICE_NAME
#define CONFIG_PINSCOPE_DEVICE_NAME ""
#endif
#ifndef CONFIG_PINSCOPE_LOCAL_PORT
#define CONFIG_PINSCOPE_LOCAL_PORT 80
#endif
#ifndef CONFIG_PINSCOPE_TASK_PRIORITY
#define CONFIG_PINSCOPE_TASK_PRIORITY 5
#endif
#ifndef CONFIG_PINSCOPE_METRICS_INTERVAL
#define CONFIG_PINSCOPE_METRICS_INTERVAL 60
#endif
#ifndef CONFIG_PINSCOPE_OTA_TRIAL_S
#define CONFIG_PINSCOPE_OTA_TRIAL_S 300
#endif
#ifndef CONFIG_PINSCOPE_POLL_MS
#define CONFIG_PINSCOPE_POLL_MS 250
#endif
/* Without Kconfig both features default to on. */
#ifdef CONFIG_PINSCOPE_KCONFIG
#ifdef CONFIG_PINSCOPE_LOG_CAPTURE
#define DEFAULT_LOG_CAPTURE 1
#else
#define DEFAULT_LOG_CAPTURE 0
#endif
#ifdef CONFIG_PINSCOPE_SYSTEM_METRICS
#define DEFAULT_SYSTEM_METRICS 1
#else
#define DEFAULT_SYSTEM_METRICS 0
#endif
#else
#define DEFAULT_LOG_CAPTURE    1
#define DEFAULT_SYSTEM_METRICS 1
#endif

/* ---- fixed sizes: the RAM Pinscope needs is the sum of these ---- */

#define STREAMS  2    /* tunnelled connections open at once */
#define FRAME    1024 /* payload bytes per frame to the service */
#define RX_FRAME 512  /* ... and from it: requests, firmware chunks; the server
                       * sends no more than the device announces in HELLO */
#define OUTBOX   2048 /* logs and metrics waiting to be sent */

/* ---- protocol ---- */

#define LEN_HDR 2  /* message length in front of every encrypted frame */
#define PS_HDR  3  /* type + stream id */
#define TAG_LEN 16

enum {
    T_WELCOME = 0x01,
    T_OPEN    = 0x02,
    T_DATA    = 0x03,
    T_CLOSE   = 0x04,
    T_WINDOW  = 0x05,
    T_PING    = 0x06,
    T_PONG    = 0x07,
    T_HELLO   = 0x08,
    T_REFUSE  = 0x09,
    T_METRIC  = 0x10,
    T_LOG     = 0x11,
    T_EVENT   = 0x12,
    T_OTA_BEGIN  = 0x20,
    T_OTA_DATA   = 0x21,
    T_OTA_END    = 0x22,
    T_OTA_STATUS = 0x23,
};

#define RX_TIMEOUT_MS      35000 /* server pings every 10 s */
#define IO_TIMEOUT_MS      10000
#define HANDSHAKE_MS       10000
#define LOCAL_CONNECT_MS   3000
#define LOCAL_BURST        4     /* frames taken from one stream before looking elsewhere */
#define HOLD_MS            20    /* how long a partial frame waits for the rest */
#define STALL_TIMEOUT_MS   5000
#define BACKOFF_MIN_MS     1000
#define BACKOFF_MAX_MS     60000
#define BACKOFF_REJECT_MS  120000
#define METRIC_NAME_MAX    32

enum { ST_FREE = 0, ST_CONNECTING, ST_OPEN };

typedef struct {
    uint16_t id;
    uint8_t  state;
    int      h;        /* the connection to the local server (ps_local_*) */
    uint32_t credit;   /* bytes we may still send to the cloud */
    uint32_t deadline; /* local connect deadline */
    bool     bulk;     /* the last frame from it was full: more is on the way */
    bool     held;     /* a piece smaller than a frame is waiting for the rest */
    uint32_t held_since;
} stream_t;

static struct {
    pinscope_config_t cfg;
    char     host[64];
    uint16_t port;
    uint8_t  server_key[32]; /* the service's static public key */
    uint8_t  k_tx[32], k_rx[32];
    uint64_t n_tx, n_rx;
    uint16_t sys_mask;
    uint32_t sys_interval_ms;
    char     device_id[41];
    char     token[65];
    bool     token_new; /* issued in WELCOME, not stored yet */
    char     url[96];

    volatile uint8_t state;
    volatile bool    stop, kick, running;
    uint32_t hold_ms;    /* a stream is waiting this much longer for the rest of a frame */

    int      fd;         /* connection to the service, -1 when there is none */
    bool     io_err;
    stream_t streams[STREAMS];

    uint8_t  rx[PS_HDR + RX_FRAME + TAG_LEN + 1]; /* one incoming message, +1 for a NUL */
    uint16_t rx_len, rx_have;
    uint8_t  rx_hdr[LEN_HDR], rx_stage;
    uint8_t  tx[LEN_HDR + PS_HDR + FRAME + TAG_LEN];

    /* Data from the cloud that the local socket did not accept yet. It points
     * into rx, so nothing more is read from the cloud until it is flushed. */
    const uint8_t *pend;
    uint16_t  pend_len;
    stream_t *pend_s;
    uint32_t  pend_since;

    uint32_t last_rx, next_sys;

    bool     ota_active;
    uint32_t ota_size, ota_got, ota_reported;

    /* Outbox: ring of [type][len][payload] records, oldest dropped first. */
    uint8_t  ob[OUTBOX];
    uint16_t ob_head, ob_used;
    volatile bool ob_on;
} g;

static void set_state(pinscope_state_t s) { g.state = (uint8_t)s; }


static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ---- outbox ---- */

static void ob_put_locked(uint8_t type, const uint8_t *data, uint8_t len)
{
    uint16_t need = (uint16_t)(2 + len);
    if (!g.ob_on) {
        return;
    }
    while ((uint16_t)(OUTBOX - g.ob_used) < need) {
        uint16_t drop = (uint16_t)(2 + g.ob[(g.ob_head + 1) % OUTBOX]);
        g.ob_head = (uint16_t)((g.ob_head + drop) % OUTBOX);
        g.ob_used = (uint16_t)(g.ob_used - drop);
    }
    uint16_t pos = (uint16_t)((g.ob_head + g.ob_used) % OUTBOX);
    g.ob[pos] = type;
    g.ob[(pos + 1) % OUTBOX] = len;
    for (uint16_t i = 0; i < len; i++) {
        g.ob[(pos + 2 + i) % OUTBOX] = data[i];
    }
    g.ob_used = (uint16_t)(g.ob_used + need);
}

void ps_log_locked(const char *text, size_t len)
{
    while (len) {
        uint8_t n = len > 255 ? 255 : (uint8_t)len;
        ob_put_locked(T_LOG, (const uint8_t *)text, n);
        text += n;
        len -= n;
    }
}

void pinscope_log(const char *text, size_t len)
{
    if (!g.ob_on || !text) {
        return;
    }
    ps_port_lock();
    ps_log_locked(text, len);
    ps_port_unlock();
}

void pinscope_metric(const char *name, double value)
{
    uint8_t rec[1 + METRIC_NAME_MAX + 8];
    if (!g.ob_on || !name) {
        return;
    }
    size_t n = strlen(name);
    if (n == 0) {
        return;
    }
    if (n > METRIC_NAME_MAX) {
        n = METRIC_NAME_MAX;
    }
    rec[0] = (uint8_t)n;
    memcpy(rec + 1, name, n);
    uint64_t bits;
    memcpy(&bits, &value, 8);
    for (int i = 0; i < 8; i++) { /* IEEE-754 double, little-endian */
        rec[1 + n + i] = (uint8_t)(bits >> (8 * i));
    }
    ps_port_lock();
    ob_put_locked(T_METRIC, rec, (uint8_t)(1 + n + 8));
    ps_port_unlock();
}

/* ---- connection to the service ---- */

/* Writes everything or fails. */
static int conn_write(const void *buf, size_t len)
{
    const char *p = buf;
    uint32_t start = ps_port_millis();
    while (len) {
        int n = (int)send(g.fd, p, (int)len, PS_SEND_FLAGS);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n == 0 || !ps_sock_wouldblock() || (uint32_t)(ps_port_millis() - start) > IO_TIMEOUT_MS) {
            g.io_err = true;
            return -1;
        }
        fd_set w;
        struct timeval tv = { 0, 100 * 1000 };
        FD_ZERO(&w);
        FD_SET(g.fd, &w);
        select(g.fd + 1, NULL, &w, NULL, &tv);
    }
    return 0;
}

/* >0 bytes read, 0 nothing available right now, <0 closed or failed. */
static int conn_read(void *buf, size_t len)
{
    int n = (int)recv(g.fd, buf, (int)len, 0);
    if (n > 0) {
        return n;
    }
    return (n < 0 && ps_sock_wouldblock()) ? 0 : -1;
}

static int conn_wait(uint32_t ms)
{
    fd_set r;
    struct timeval tv = { (long)(ms / 1000), (long)(ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_SET(g.fd, &r);
    return select(g.fd + 1, &r, NULL, NULL, &tv) > 0;
}

static int conn_open(void)
{
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g.port);
    /* rx is idle until connected: the resolver may use it */
    if (!ps_port_network_ready() ||
        ps_port_resolve(g.host, g.rx, sizeof(g.rx), (uint32_t *)&addr.sin_addr.s_addr) != 0) {
        return -1;
    }
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    int rc = -1;
    if (fd >= 0) {
        ps_set_nonblock(fd);
        rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (rc != 0 && ps_sock_inprogress()) {
            fd_set w, e;
            struct timeval tv = { IO_TIMEOUT_MS / 1000, 0 };
            int err = -1;
            socklen_t len = sizeof(err);
            FD_ZERO(&w);
            FD_ZERO(&e);
            FD_SET(fd, &w);
            FD_SET(fd, &e);
            if (select(fd + 1, NULL, &w, &e, &tv) > 0 && !FD_ISSET(fd, &e)) {
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            }
            rc = err == 0 ? 0 : -1;
        }
    }
    if (rc != 0) {
        if (fd >= 0) {
            ps_closesocket(fd);
        }
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    g.fd = fd;
    return 0;
}

/* ---- framing: [length:2][encrypted message][tag:16] ---- */

/* Body of `blen` bytes must already sit at g.tx + LEN_HDR + PS_HDR. */
static int send_frame(uint8_t type, uint16_t id, uint16_t blen)
{
    uint8_t *p = g.tx + LEN_HDR;
    uint16_t len = (uint16_t)(PS_HDR + blen), total = (uint16_t)(len + TAG_LEN);
    p[0] = type;
    p[1] = (uint8_t)(id >> 8);
    p[2] = (uint8_t)id;
    ps_aead_seal(g.k_tx, g.n_tx++, NULL, 0, p, len);
    g.tx[0] = (uint8_t)(total >> 8);
    g.tx[1] = (uint8_t)total;
    return conn_write(g.tx, (size_t)total + LEN_HDR);
}

/* 1 = a complete message is in g.rx, 0 = need more bytes, -1 = protocol/io error */
static int rx_pump(void)
{
    for (;;) {
        uint8_t *dst = g.rx_stage ? g.rx : g.rx_hdr;
        uint16_t need = g.rx_stage ? g.rx_len : LEN_HDR;
        while (g.rx_have < need) {
            int n = conn_read(dst + g.rx_have, (size_t)(need - g.rx_have));
            if (n <= 0) {
                return n;
            }
            g.rx_have = (uint16_t)(g.rx_have + n);
            g.last_rx = ps_port_millis();
        }
        if (g.rx_stage) {
            g.rx_stage = 0;
            g.rx_have = 0;
            g.rx_len = (uint16_t)(g.rx_len - TAG_LEN);
            if (ps_aead_open(g.k_rx, g.n_rx++, NULL, 0, g.rx, g.rx_len) != 0) {
                return -1; /* not from our server, or damaged */
            }
            return 1;
        }
        g.rx_len = (uint16_t)((g.rx_hdr[0] << 8) | g.rx_hdr[1]);
        if (g.rx_len < TAG_LEN || g.rx_len > sizeof(g.rx) - 1) {
            return -1;
        }
        g.rx_stage = 1;
        g.rx_have = 0;
    }
}

/* ---- streams ---- */

static stream_t *stream_find(uint16_t id)
{
    for (int i = 0; i < STREAMS; i++) {
        if (g.streams[i].state != ST_FREE && g.streams[i].id == id) {
            return &g.streams[i];
        }
    }
    return NULL;
}

enum { CLOSE_NORMAL = 0, CLOSE_BUSY = 1, CLOSE_REFUSED = 2 };

static void send_close(uint16_t id, uint8_t reason)
{
    if (g.fd >= 0 && !g.io_err) {
        g.tx[LEN_HDR + PS_HDR] = reason;
        send_frame(T_CLOSE, id, 1);
    }
}

static void stream_free(stream_t *s, bool notify)
{
    if (s->state == ST_FREE) {
        return;
    }
    ps_local_close(s->h);
    s->state = ST_FREE;
    if (g.pend_s == s) {
        g.pend_s = NULL;
        g.pend_len = 0;
    }
    if (notify) {
        send_close(s->id, CLOSE_NORMAL);
    }
}

static void flush_pending(void)
{
    stream_t *s = g.pend_s;
    int n = ps_local_write(s->h, g.pend, g.pend_len);
    if (n > 0) {
        g.pend += n;
        g.pend_len = (uint16_t)(g.pend_len - n);
        g.pend_since = ps_port_millis();
        if (g.pend_len == 0) {
            g.pend_s = NULL;
        }
    } else if (n < 0) {
        stream_free(s, true);
    }
}

/* One frame of what the local server has sent. True when the frame was full,
 * so more is probably waiting. */
static bool pump_local(stream_t *s)
{
    /* Servers often write a response in small pieces (a chunk header, the
     * data, a CRLF). Everything already queued goes into one frame: one
     * encryption and one packet on the air instead of several. */
    uint32_t max = s->credit < FRAME ? s->credit : FRAME, got = 0;
    char *buf = (char *)g.tx + LEN_HDR + PS_HDR;
    int n = ps_local_read(s->h, buf, (size_t)max); /* all that is queued, up to a frame */
    bool closed = n < 0;
    if (n > 0) {
        got = (uint32_t)n;
    }
    if (got) {
        s->credit -= got;
        s->bulk = got == FRAME;
        send_frame(T_DATA, s->id, (uint16_t)got);
    }
    if (closed) {
        stream_free(s, true); /* local server closed the connection */
        return false;
    }
    return got == FRAME && s->credit && !g.io_err;
}

static void on_open(uint16_t id, const uint8_t *b, uint16_t n)
{
    uint16_t port = n >= 2 ? (uint16_t)((b[0] << 8) | b[1]) : 0;
    stream_t *s = NULL;
    bool allowed = false;

    if (port == 0) {
        port = g.cfg.ports[0];
    }
    for (int i = 0; i < PINSCOPE_MAX_PORTS; i++) {
        allowed = allowed || (g.cfg.ports[i] && g.cfg.ports[i] == port);
    }
    if (allowed && !stream_find(id)) {
        for (int i = 0; i < STREAMS && !s; i++) {
            if (g.streams[i].state == ST_FREE) {
                s = &g.streams[i];
            }
        }
    }
    if (!allowed) {
        send_close(id, CLOSE_REFUSED);
        return;
    }
    /* Out of slots or sockets: the server lowers its limit and retries. */
    int h = s ? ps_local_open(port) : -1;
    if (h < 0) {
        send_close(id, CLOSE_BUSY);
        return;
    }
    s->id = id;
    s->h = h;
    s->credit = n >= 6 ? be32(b + 2) : 4096;
    s->state = ST_CONNECTING;
    s->bulk = s->held = false;
    s->deadline = ps_port_millis() + LOCAL_CONNECT_MS;
}

static void on_welcome(char *text)
{
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        if (strncmp(line, "token=", 6) == 0 && strcmp(line + 6, g.token) != 0) {
            snprintf(g.token, sizeof(g.token), "%s", line + 6);
            g.token_new = true;
        } else if (strncmp(line, "url=", 4) == 0) {
            snprintf(g.url, sizeof(g.url), "%s", line + 4);
        }
        line = end ? end + 1 : NULL;
    }
}

/* ---- firmware update ---- */

enum { OTA_PROGRESS = 1, OTA_REBOOTING = 2, OTA_FAILED = 3 };

static void ota_status(uint8_t state, const char *text)
{
    uint8_t *b = g.tx + LEN_HDR + PS_HDR;
    size_t n = strlen(text);
    b[0] = state;
    b[1] = (uint8_t)(g.ota_got >> 24);
    b[2] = (uint8_t)(g.ota_got >> 16);
    b[3] = (uint8_t)(g.ota_got >> 8);
    b[4] = (uint8_t)g.ota_got;
    memcpy(b + 5, text, n);
    send_frame(T_OTA_STATUS, 0, (uint16_t)(5 + n));
}

static void ota_fail(const char *why)
{
    if (g.ota_active) {
        ps_port_ota_abort();
        g.ota_active = false;
    }
    ps_port_logf(PS_LOG_ERROR, "firmware update failed: %s", why);
    ota_status(OTA_FAILED, why);
}

static void on_ota(uint8_t type, const uint8_t *body, uint16_t blen)
{
    if (type == T_OTA_BEGIN && blen >= 4) {
        if (g.ota_active) {
            ps_port_ota_abort();
        }
        g.ota_active = false;
        g.ota_size = be32(body);
        g.ota_got = g.ota_reported = 0;
        if (ps_port_ota_begin(g.ota_size) != 0) {
            ota_fail("no spare OTA partition large enough, or updates are disabled");
            return;
        }
        g.ota_active = true;
        ps_port_logf(PS_LOG_INFO, "firmware update started, %u bytes", (unsigned)g.ota_size);
        ota_status(OTA_PROGRESS, "");
    } else if (type == T_OTA_DATA && g.ota_active) {
        if (ps_port_ota_write(body, blen) != 0) {
            ota_fail("writing to flash failed");
            return;
        }
        g.ota_got += blen;
        if (g.ota_got - g.ota_reported >= 32768) {
            g.ota_reported = g.ota_got;
            ota_status(OTA_PROGRESS, "");
        }
    } else if (type == T_OTA_END && g.ota_active) {
        if (g.ota_got != g.ota_size) {
            ota_fail("the image arrived incomplete");
        } else if (ps_port_ota_end() != 0) {
            g.ota_active = false;
            ota_fail("the image did not pass verification");
        } else {
            g.ota_active = false;
            ps_port_logf(PS_LOG_INFO, "firmware update written, restarting");
            ota_status(OTA_REBOOTING, "");
            ps_port_sleep(500);
            ps_port_restart();
        }
    }
}

static int dispatch(void)
{
    uint8_t *p = g.rx;
    uint16_t n = g.rx_len;

    if (n < PS_HDR) {
        return 0;
    }
    uint16_t id = (uint16_t)((p[1] << 8) | p[2]);
    uint8_t *body = p + PS_HDR;
    uint16_t blen = (uint16_t)(n - PS_HDR);
    stream_t *s = stream_find(id);

    switch (p[0]) {
    case T_WELCOME:
        body[blen] = '\0'; /* rx has one spare byte */
        on_welcome((char *)body);
        break;
    case T_OPEN:
        on_open(id, body, blen);
        break;
    case T_DATA:
        if (s && blen) {
            s->bulk = false; /* a new request: its answer starts at once */
            g.pend = body;
            g.pend_len = blen;
            g.pend_s = s;
            g.pend_since = ps_port_millis();
            if (s->state == ST_OPEN) {
                flush_pending();
            }
        }
        break;
    case T_CLOSE:
        if (s) {
            stream_free(s, false);
        }
        break;
    case T_WINDOW:
        if (s && blen >= 4) {
            s->credit += be32(body);
        }
        break;
    case T_PING:
        memcpy(g.tx + LEN_HDR + PS_HDR, body, blen);
        return send_frame(T_PONG, 0, blen);
    case T_OTA_BEGIN:
    case T_OTA_DATA:
    case T_OTA_END:
        on_ota(p[0], body, blen);
        break;
    default:
        break; /* unknown types are ignored for forward compatibility */
    }
    return g.io_err ? -1 : 0;
}

/* Send queued logs and metrics, a few frames per call to stay fair. */
static int ob_flush(void)
{
    for (int k = 0; k < 4; k++) {
        uint8_t *body = g.tx + LEN_HDR + PS_HDR, type = 0;
        uint16_t total = 0;

        ps_port_lock();
        while (g.ob_used) {
            uint8_t t = g.ob[g.ob_head];
            uint8_t l = g.ob[(g.ob_head + 1) % OUTBOX];
            if ((total && t != type) || total + l > FRAME) {
                break;
            }
            type = t;
            for (uint16_t i = 0; i < l; i++) {
                body[total + i] = g.ob[(g.ob_head + 2 + i) % OUTBOX];
            }
            total = (uint16_t)(total + l);
            g.ob_head = (uint16_t)((g.ob_head + 2 + l) % OUTBOX);
            g.ob_used = (uint16_t)(g.ob_used - 2 - l);
        }
        ps_port_unlock();

        if (!type) {
            break;
        }
        if (send_frame(type, 0, total) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ---- handshake ---- */

static void ports_text(char *ports, size_t cap)
{
    size_t pl = 0;
    ports[0] = '\0';
    for (int i = 0; i < PINSCOPE_MAX_PORTS; i++) {
        if (g.cfg.ports[i] && pl < cap - 7) {
            pl += (size_t)snprintf(ports + pl, cap - pl, pl ? ",%u" : "%u", (unsigned)g.cfg.ports[i]);
        }
    }
}

static int read_exact(uint8_t *buf, size_t len, uint32_t t0)
{
    size_t have = 0;
    while (have < len) {
        int n = conn_read(buf + have, len - have);
        if (n < 0 || (uint32_t)(ps_port_millis() - t0) > HANDSHAKE_MS) {
            return -1;
        }
        if (n == 0) {
            conn_wait(250);
            continue;
        }
        have += (size_t)n;
    }
    return 0;
}

static int dispatch(void);

/* Noise_NK_25519_ChaChaPoly_SHA256: the server's public key is known in
 * advance, so one message each way proves it is our server and leaves both
 * sides with fresh keys. Who the device is travels afterwards, encrypted. */
typedef struct {
    uint8_t h[32], ck[32], k[32];
} noise_t;

static void mix_hash(noise_t *s, const uint8_t *data, size_t len)
{
    ps_sha256_t c;
    ps_sha256_init(&c);
    ps_sha256_update(&c, s->h, 32);
    ps_sha256_update(&c, data, len);
    ps_sha256_final(&c, s->h);
}

/* 0 = mixed in, -1 = the other side sent a point that gives no secret */
static int mix_dh(noise_t *s, const uint8_t priv[32], const uint8_t pub[32])
{
    uint8_t dh[32], any = 0;
    ps_x25519(dh, priv, pub);
    for (int i = 0; i < 32; i++) {
        any |= dh[i];
    }
    ps_hkdf2(s->ck, dh, 32, s->ck, s->k);
    memset(dh, 0, sizeof(dh));
    return any ? 0 : -1;
}

/* Who the device is, written straight into the frame. Kept out of
 * handshake() so that its stack is gone before the answer is handled. */
static PS_NOINLINE int send_hello(void)
{
    char *body = (char *)g.tx + LEN_HDR + PS_HDR, ports[32];
    ports_text(ports, sizeof(ports));
    int n = snprintf(body, FRAME,
                     "key=%s\ndevice=%s\ntoken=%s\nname=%s\n"
                     "info=sdk=" PINSCOPE_VERSION ";streams=%u;frame=%u;ports=%s;ota=%d;",
                     g.cfg.project_key, g.device_id, g.token, g.cfg.device_name,
                     (unsigned)STREAMS, (unsigned)RX_FRAME, ports, ps_port_ota_supported() ? 1 : 0);
    if (n < 0 || n >= FRAME - 1) {
        return -1;
    }
    ps_port_info(body + n, (size_t)(FRAME - 1 - n));
    n += (int)strlen(body + n);
    if (body[n - 1] == ';') {
        n--; /* no platform info */
    }
    body[n++] = '\n';
    return send_frame(T_HELLO, 0, (uint16_t)n);
}

/* 0 = admitted, -2 = credentials refused, -1 = anything else */
static int handshake(void)
{
    static const char name[33] = "Noise_NK_25519_ChaChaPoly_SHA256";
    static const uint8_t magic[4] = { 'P', 'S', 'L', '1' };
    const uint8_t *server = g.server_key;
    uint8_t e[32], msg[4 + 32 + TAG_LEN], *pub = msg + 4, *tag = msg + 36;
    noise_t s;
    int rc = -1;

    memcpy(s.h, name, 32);
    memcpy(s.ck, name, 32);
    mix_hash(&s, magic, sizeof(magic));
    mix_hash(&s, server, 32);

    /* -> e, es */
    ps_port_random(e, sizeof(e));
    memcpy(msg, magic, 4);
    ps_x25519(pub, e, NULL);
    mix_hash(&s, pub, 32);
    if (mix_dh(&s, e, server) != 0) {
        goto done;
    }
    ps_aead_seal(s.k, 0, s.h, 32, tag, 0);
    mix_hash(&s, tag, TAG_LEN);
    uint32_t t0 = ps_port_millis();
    if (conn_write(msg, sizeof(msg)) != 0) {
        goto done;
    }

    /* <- e, ee */
    if (read_exact(msg, 32 + TAG_LEN, t0) != 0) {
        goto done;
    }
    mix_hash(&s, msg, 32);
    if (mix_dh(&s, e, msg) != 0 || ps_aead_open(s.k, 0, s.h, 32, msg + 32, 0) != 0) {
        ps_port_logf(PS_LOG_ERROR, "the server did not prove its identity");
        goto done;
    }
    ps_hkdf2(s.ck, NULL, 0, g.k_tx, g.k_rx);
    g.n_tx = g.n_rx = 0;

    if (send_hello() != 0) {
        goto done;
    }
    for (;;) {
        int r = rx_pump();
        if (r < 0 || (uint32_t)(ps_port_millis() - t0) > HANDSHAKE_MS) {
            goto done;
        }
        if (r == 0) {
            conn_wait(250);
            continue;
        }
        if (g.rx_len >= PS_HDR && g.rx[0] == T_REFUSE) {
            int why = g.rx_len > PS_HDR ? g.rx[PS_HDR] : 0;
            ps_port_logf(PS_LOG_ERROR, "the server refused this device (%d)", why);
            rc = why == 1 ? -2 : -1;
        } else {
            rc = dispatch() < 0 ? -1 : 0;
        }
        break;
    }
done:
    memset(e, 0, sizeof(e));
    memset(&s, 0, sizeof(s));
    return rc;
}

/* A crash recorded before the reboot is delivered first and only forgotten
 * once the server has it. */
static void report_crash(void)
{
    int n = ps_port_crash_read((char *)g.tx + LEN_HDR + PS_HDR, FRAME);
    if (n > 0 && send_frame(T_EVENT, 0, (uint16_t)(n > FRAME ? FRAME : n)) == 0) {
        ps_port_crash_clear();
    }
}

static void run_session(void)
{
    int cfd = g.fd;
    uint32_t poll_us = (CONFIG_PINSCOPE_POLL_MS > 999 ? 999 : CONFIG_PINSCOPE_POLL_MS) * 1000UL;

    g.last_rx = ps_port_millis();
    g.next_sys = g.last_rx + 2000;

    while (!g.stop && !g.io_err) {
        uint32_t now = ps_port_millis();
        if ((uint32_t)(now - g.last_rx) > RX_TIMEOUT_MS) {
            ps_port_logf(PS_LOG_WARN, "server went silent, reconnecting");
            return;
        }
        if (g.sys_mask && (int32_t)(now - g.next_sys) >= 0) {
            ps_port_sys_metrics(g.sys_mask);
            g.next_sys = now + g.sys_interval_ms;
        }
        if (ob_flush() != 0) {
            return;
        }
        ps_local_flush();

        fd_set rf, wf, ef;
        int maxfd = -1;
        FD_ZERO(&rf);
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        if (!g.pend_len) {
            FD_SET(cfd, &rf);
            maxfd = cfd;
        }
        for (int i = 0; i < STREAMS; i++) {
            stream_t *s = &g.streams[i];
            if (s->state == ST_FREE) {
                continue;
            }
            ps_local_watch(s->h, s->state == ST_CONNECTING, s->state == ST_OPEN && s->credit, g.pend_s == s,
                           &rf, &wf, &ef, &maxfd);
        }
        /* News that came while the loop was busy must not wait for a timeout. */
        struct timeval tv = { 0, ps_local_ready() ? 0 : g.hold_ms ? (long)g.hold_ms * 1000 : (long)poll_us };
        g.hold_ms = 0;
        if (select(maxfd + 1, &rf, &wf, &ef, &tv) < 0) {
            ps_port_sleep(20);
            continue;
        }

        now = ps_port_millis();
        ps_local_poll(&rf);
        for (int i = 0; i < STREAMS; i++) {
            stream_t *s = &g.streams[i];
            if (s->state == ST_CONNECTING) {
                int st = ps_local_state(s->h);
                if (st > 0) {
                    s->state = ST_OPEN;
                } else if (st < 0 || (int32_t)(now - s->deadline) >= 0) {
                    stream_free(s, true);
                }
            }
            if (s->state == ST_OPEN) {
                if (g.pend_s == s) {
                    flush_pending();
                }
                /* Whatever woke the loop, take what the local server has
                 * sent, a few frames at a time so no stream waits long. */
                for (int burst = 0; s->state == ST_OPEN && s->credit && burst < LOCAL_BURST; burst++) {
                    /* In the middle of a large answer the server's pieces do
                     * not line up with frames. A leftover smaller than a
                     * frame waits for what follows instead of going out as a
                     * tiny frame of its own: the loop is woken when a whole
                     * frame is there, and sends what there is once the
                     * server has been quiet for HOLD_MS. */
                    int avail = ps_local_avail(s->h);
                    uint32_t want = s->credit < FRAME ? s->credit : FRAME;
                    if (s->bulk && avail > 0 && (uint32_t)avail < want) {
                        if (!s->held) {
                            s->held = true;
                            s->held_since = now;
                        }
                        uint32_t waited = (uint32_t)(now - s->held_since);
                        if (waited < HOLD_MS) {
                            if (!g.hold_ms || HOLD_MS - waited < g.hold_ms) {
                                g.hold_ms = HOLD_MS - waited;
                            }
                            ps_local_want(s->h, want);
                            break;
                        }
                    }
                    s->held = false;
                    ps_local_want(s->h, 1);
                    if (!pump_local(s)) {
                        break;
                    }
                }
            }
        }
        /* A local server that stopped reading must not freeze the tunnel. */
        if (g.pend_len && (uint32_t)(now - g.pend_since) > STALL_TIMEOUT_MS) {
            stream_free(g.pend_s, true);
        }

        if (!g.pend_len && FD_ISSET(cfd, &rf)) {
            int budget = 8;
            do {
                int r = rx_pump();
                if (r < 0 || (r > 0 && dispatch() < 0)) {
                    return;
                }
                if (r == 0) {
                    break;
                }
            } while (!g.pend_len && --budget);
        }
    }
}

static void session_cleanup(void)
{
    for (int i = 0; i < STREAMS; i++) {
        stream_free(&g.streams[i], false);
    }
    if (g.fd >= 0) {
        ps_closesocket(g.fd);
        g.fd = -1;
    }
    if (g.ota_active) { /* connection lost mid-update: nothing was switched */
        ps_port_ota_abort();
        g.ota_active = false;
    }
    g.io_err = false;
    g.rx_stage = 0;
    g.rx_have = 0;
    g.pend_len = 0;
    g.pend_s = NULL;
    memset(g.k_tx, 0, sizeof(g.k_tx));
    memset(g.k_rx, 0, sizeof(g.k_rx));
}

/* A new image that cannot reach the server would otherwise keep running, out
 * of reach, until something else reset the device. Give it a fixed time, then
 * restart so the bootloader goes back to the image that worked. */
static void give_up_on_trial_image(void)
{
    if (CONFIG_PINSCOPE_OTA_TRIAL_S > 0 && ps_port_ota_on_trial() &&
        ps_port_millis() > (uint32_t)CONFIG_PINSCOPE_OTA_TRIAL_S * 1000UL) {
        ps_port_logf(PS_LOG_ERROR, "the updated firmware did not reach the server in %d s, "
                     "restarting into the previous one", CONFIG_PINSCOPE_OTA_TRIAL_S);
        ps_port_sleep(200);
        ps_port_restart();
    }
}

static void task_main(void *arg)
{
    uint32_t backoff = BACKOFF_MIN_MS;
    (void)arg;

    while (!g.stop) {
        uint32_t started = ps_port_millis(), jitter;
        int rc;

        set_state(PINSCOPE_CONNECTING);
        rc = conn_open() == 0 ? handshake() : -1;
        if (rc == 0) {
            set_state(PINSCOPE_ONLINE);
            if (g.token_new) {
                g.token_new = false;
                if (ps_port_kv_set("token", g.token) != 0) {
                    ps_port_logf(PS_LOG_WARN, "could not store the device credential");
                }
            }
            ps_port_logf(PS_LOG_INFO, "online at %s", g.url);
            /* Reaching the server is the proof that a freshly updated image works. */
            ps_port_ota_confirm();
            report_crash();
            run_session();
            if ((uint32_t)(ps_port_millis() - started) > 10000) {
                backoff = BACKOFF_MIN_MS;
            }
        }
        session_cleanup();
        if (g.stop) {
            break;
        }

        uint32_t wait = backoff;
        if (rc == -2) {
            set_state(PINSCOPE_REJECTED);
            ps_port_logf(PS_LOG_ERROR,
                         "credentials refused - check the project key, or reset this "
                         "device's credential in the dashboard");
            wait = BACKOFF_REJECT_MS;
        } else {
            set_state(PINSCOPE_CONNECTING);
            backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
        }
        ps_port_random(&jitter, sizeof(jitter));
        wait += jitter % (wait / 4 + 1);
        g.kick = false;
        for (uint32_t t = 0; t < wait && !g.stop && !g.kick; t += 100) {
            ps_port_sleep(100);
            if (t % 1000 == 0) {
                give_up_on_trial_image();
            }
        }
    }
    set_state(PINSCOPE_STOPPED);
    g.running = false;
}

/* ---- public API ---- */

/* Set only by the simulator and the test suite, which run against a server
 * on the same machine. */
const char *ps_test_server_url;

static int hex_nibble(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* psl://host:port/<64 hex digits> */
static int parse_url(const char *u)
{
    if (strncmp(u, "psl://", 6) != 0) {
        return -1;
    }
    u += 6;
    size_t n = strcspn(u, ":/");
    if (n == 0 || n >= sizeof(g.host) || u[n] != ':') {
        return -1;
    }
    memcpy(g.host, u, n);
    g.host[n] = '\0';
    g.port = (uint16_t)atoi(u + n + 1);
    const char *key = strchr(u, '/');
    if (!g.port || !key || strlen(key + 1) != 64) {
        return -1;
    }
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(key[1 + 2 * i]), lo = hex_nibble(key[2 + 2 * i]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        g.server_key[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

static const char *pick(const char *value, const char *fallback)
{
    return (value && value[0]) ? value : fallback;
}

static bool opt(pinscope_opt_t o, bool fallback)
{
    return o == PINSCOPE_DEFAULT ? fallback : o == PINSCOPE_ON;
}

int pinscope_start(const pinscope_config_t *config)
{
    if (g.running) {
        return 0;
    }
    memset(&g, 0, sizeof(g));
    if (config) {
        g.cfg = *config;
    }
    pinscope_config_t *c = &g.cfg;
    c->project_key = pick(c->project_key, CONFIG_PINSCOPE_PROJECT_KEY);
    c->device_name = pick(c->device_name, CONFIG_PINSCOPE_DEVICE_NAME);
    if (!c->ports[0]) {
        c->ports[0] = CONFIG_PINSCOPE_LOCAL_PORT;
    }
    if (!c->task_priority) {
        c->task_priority = CONFIG_PINSCOPE_TASK_PRIORITY;
    }
    bool logs = opt(c->log_capture, DEFAULT_LOG_CAPTURE);
    if (!c->metrics_interval_s) {
        c->metrics_interval_s = CONFIG_PINSCOPE_METRICS_INTERVAL;
    }
    g.sys_interval_ms = c->metrics_interval_s * 1000UL;

    if (ps_port_init() != 0) {
        return -1;
    }
    if (opt(c->system_metrics, DEFAULT_SYSTEM_METRICS)) {
        g.sys_mask = c->metrics ? c->metrics : ps_port_default_metrics();
    }
    if (!c->project_key[0]) {
        ps_port_logf(PS_LOG_ERROR, "the project key is missing: set it in menuconfig under Pinscope");
        return -1;
    }
    if (parse_url(ps_test_server_url ? ps_test_server_url : PINSCOPE_SERVICE_URL) != 0) {
        ps_port_logf(PS_LOG_ERROR, "no valid service address (psl://host:port/key)");
        return -1;
    }

    g.fd = -1;
    g.ob_on = true;

    ps_port_device_id(g.device_id, sizeof(g.device_id));
    if (ps_port_kv_get("token", g.token, sizeof(g.token)) != 0) {
        g.token[0] = '\0';
    }
    ps_port_log_capture(logs);

    g.running = true;
    set_state(PINSCOPE_CONNECTING);
    if (ps_port_task_start(task_main, PS_TASK_STACK, c->task_priority) != 0) {
        g.running = false;
        set_state(PINSCOPE_STOPPED);
        return -1;
    }
    return 0;
}

void pinscope_stop(void)
{
    if (!g.running) {
        return;
    }
    g.stop = true;
    for (int i = 0; i < 150 && g.running; i++) {
        ps_port_sleep(100);
    }
    if (g.running) {
        return; /* task is stuck in a connect */
    }
    ps_port_log_capture(false);
    ps_port_lock();
    g.ob_on = false;
    ps_port_unlock();
}

pinscope_state_t pinscope_state(void) { return (pinscope_state_t)g.state; }

const char *pinscope_url(void) { return g.url; }

void pinscope_kick(void) { g.kick = true; }
