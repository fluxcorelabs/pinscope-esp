/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 FluxCore Labs. See LICENSE: for use with the Pinscope service only. */
/*
 * Pinscope core: WebSocket client, stream multiplexer and outbox.
 * Portable C, single task, no allocation after pinscope_start().
 *
 * See docs/PROTOCOL.md for the wire format.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pinscope.h"
#include "ps_port.h"

/* ---- build-time defaults (Kconfig on ESP-IDF, plain macros elsewhere) ---- */

/* The Pinscope service. Not a setting: the component is licensed for use with
 * this service only (see LICENSE). */
#ifndef PINSCOPE_SERVICE_URL
#define PINSCOPE_SERVICE_URL "wss://pinscope.fluxcorelabs.io"
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
#ifndef CONFIG_PINSCOPE_MAX_STREAMS
#define CONFIG_PINSCOPE_MAX_STREAMS 2
#endif
#ifndef CONFIG_PINSCOPE_MAX_FRAME
#define CONFIG_PINSCOPE_MAX_FRAME 1024
#endif
#ifndef CONFIG_PINSCOPE_OUTBOX_SIZE
#define CONFIG_PINSCOPE_OUTBOX_SIZE 3072
#endif
#ifndef CONFIG_PINSCOPE_TASK_STACK
#define CONFIG_PINSCOPE_TASK_STACK 8192
#endif
#ifndef CONFIG_PINSCOPE_TASK_PRIORITY
#define CONFIG_PINSCOPE_TASK_PRIORITY 5
#endif
#ifndef CONFIG_PINSCOPE_METRICS_INTERVAL
#define CONFIG_PINSCOPE_METRICS_INTERVAL 60
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

/* ---- protocol ---- */

#define WS_HDR 8 /* room reserved in front of every outgoing payload */
#define PS_HDR 3 /* type + stream id */

enum { OP_BIN = 0x2, OP_CLOSE = 0x8, OP_PING = 0x9, OP_PONG = 0xA };

enum {
    T_WELCOME = 0x01,
    T_OPEN    = 0x02,
    T_DATA    = 0x03,
    T_CLOSE   = 0x04,
    T_WINDOW  = 0x05,
    T_METRIC  = 0x10,
    T_LOG     = 0x11,
    T_EVENT   = 0x12,
    T_OTA_BEGIN  = 0x20,
    T_OTA_DATA   = 0x21,
    T_OTA_END    = 0x22,
    T_OTA_STATUS = 0x23,
};

#define RX_TIMEOUT_MS      35000 /* server pings every 10 s */
#define HANDSHAKE_MS       10000
#define LOCAL_CONNECT_MS   3000
#define STALL_TIMEOUT_MS   5000
#define BACKOFF_MIN_MS     1000
#define BACKOFF_MAX_MS     60000
#define BACKOFF_REJECT_MS  120000
#define METRIC_NAME_MAX    32

enum { ST_FREE = 0, ST_CONNECTING, ST_OPEN };

typedef struct {
    uint16_t id;
    uint8_t  state;
    int      fd;
    uint32_t credit;   /* bytes we may still send to the cloud */
    uint32_t deadline; /* local connect deadline */
} stream_t;

static struct {
    pinscope_config_t cfg;
    char     host[64];
    uint16_t port;
    bool     tls;
    uint16_t sys_mask;
    uint32_t sys_interval_ms;
    char     device_id[41];
    char     token[65];
    char     url[96];

    volatile uint8_t state;
    volatile bool    stop, kick, running;

    ps_conn_t *conn;
    bool       io_err;
    stream_t  *streams;

    uint8_t *rx;         /* one incoming frame payload */
    uint16_t rx_cap, rx_len, rx_have;
    uint8_t  rx_hdr[4], rx_stage, rx_op;
    uint8_t *tx;         /* WS_HDR + PS_HDR + max_frame */
    uint16_t tx_cap;

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
    uint8_t *ob;
    uint16_t ob_size, ob_head, ob_used;
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
    if (!g.ob || need > g.ob_size) {
        return;
    }
    while ((uint16_t)(g.ob_size - g.ob_used) < need) {
        uint16_t drop = (uint16_t)(2 + g.ob[(g.ob_head + 1) % g.ob_size]);
        g.ob_head = (uint16_t)((g.ob_head + drop) % g.ob_size);
        g.ob_used = (uint16_t)(g.ob_used - drop);
    }
    uint16_t pos = (uint16_t)((g.ob_head + g.ob_used) % g.ob_size);
    g.ob[pos] = type;
    g.ob[(pos + 1) % g.ob_size] = len;
    for (uint16_t i = 0; i < len; i++) {
        g.ob[(pos + 2 + i) % g.ob_size] = data[i];
    }
    g.ob_used = (uint16_t)(g.ob_used + need);
}

void pinscope_log(const char *text, size_t len)
{
    if (!g.ob || !text) {
        return;
    }
    ps_port_lock();
    while (len) {
        uint8_t n = len > 255 ? 255 : (uint8_t)len;
        ob_put_locked(T_LOG, (const uint8_t *)text, n);
        text += n;
        len -= n;
    }
    ps_port_unlock();
}

void pinscope_metric(const char *name, double value)
{
    uint8_t rec[1 + METRIC_NAME_MAX + 8];
    if (!g.ob || !name) {
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

/* ---- WebSocket framing ---- */

/* Payload of `len` bytes must already sit at g.tx + WS_HDR. */
static int ws_send(uint8_t op, uint16_t len)
{
    uint8_t *p = g.tx + WS_HDR, *h, mask[4];
    ps_port_random(mask, sizeof(mask));
    if (len < 126) {
        h = p - 6;
        h[1] = (uint8_t)(0x80 | len);
    } else {
        h = p - 8;
        h[1] = 0x80 | 126;
        h[2] = (uint8_t)(len >> 8);
        h[3] = (uint8_t)len;
    }
    h[0] = (uint8_t)(0x80 | op);
    memcpy(p - 4, mask, 4);
    for (uint16_t i = 0; i < len; i++) {
        p[i] ^= mask[i & 3];
    }
    if (ps_port_write(g.conn, h, (size_t)(p - h) + len) != 0) {
        g.io_err = true;
        return -1;
    }
    return 0;
}

/* Body of `blen` bytes must already sit at g.tx + WS_HDR + PS_HDR. */
static int send_frame(uint8_t type, uint16_t id, uint16_t blen)
{
    uint8_t *p = g.tx + WS_HDR;
    p[0] = type;
    p[1] = (uint8_t)(id >> 8);
    p[2] = (uint8_t)id;
    return ws_send(OP_BIN, (uint16_t)(blen + PS_HDR));
}

/* 1 = a complete frame is in g.rx, 0 = need more bytes, -1 = protocol/io error */
static int rx_pump(void)
{
    for (;;) {
        uint8_t *dst = g.rx_stage == 2 ? g.rx : g.rx_hdr;
        uint16_t need = g.rx_stage == 0 ? 2 : g.rx_stage == 1 ? 4 : g.rx_len;
        while (g.rx_have < need) {
            int n = ps_port_read(g.conn, dst + g.rx_have, (size_t)(need - g.rx_have));
            if (n <= 0) {
                return n;
            }
            g.rx_have = (uint16_t)(g.rx_have + n);
            g.last_rx = ps_port_millis();
        }
        if (g.rx_stage == 2) {
            g.rx_stage = 0;
            g.rx_have = 0;
            return 1;
        }
        if (g.rx_stage == 0) {
            uint8_t len7 = g.rx_hdr[1] & 0x7f;
            /* The server never fragments, never masks, never sends >64 KiB. */
            if (!(g.rx_hdr[0] & 0x80) || (g.rx_hdr[1] & 0x80) || len7 == 127) {
                return -1;
            }
            g.rx_op = g.rx_hdr[0] & 0x0f;
            if (len7 == 126) {
                g.rx_stage = 1;
                continue;
            }
            g.rx_len = len7;
        } else {
            g.rx_len = (uint16_t)((g.rx_hdr[2] << 8) | g.rx_hdr[3]);
        }
        if (g.rx_len > g.rx_cap) {
            return -1;
        }
        g.rx_stage = 2;
        g.rx_have = 0;
    }
}

/* ---- streams ---- */

static stream_t *stream_find(uint16_t id)
{
    for (int i = 0; i < g.cfg.max_streams; i++) {
        if (g.streams[i].state != ST_FREE && g.streams[i].id == id) {
            return &g.streams[i];
        }
    }
    return NULL;
}

enum { CLOSE_NORMAL = 0, CLOSE_BUSY = 1, CLOSE_REFUSED = 2 };

static void send_close(uint16_t id, uint8_t reason)
{
    if (g.conn && !g.io_err) {
        g.tx[WS_HDR + PS_HDR] = reason;
        send_frame(T_CLOSE, id, 1);
    }
}

static void stream_free(stream_t *s, bool notify)
{
    if (s->state == ST_FREE) {
        return;
    }
    ps_closesocket(s->fd);
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
    int n = (int)send(s->fd, (const char *)g.pend, g.pend_len, PS_SEND_FLAGS);
    if (n > 0) {
        g.pend += n;
        g.pend_len = (uint16_t)(g.pend_len - n);
        g.pend_since = ps_port_millis();
        if (g.pend_len == 0) {
            g.pend_s = NULL;
        }
    } else if (n < 0 && !ps_sock_wouldblock()) {
        stream_free(s, true);
    }
}

static void pump_local(stream_t *s)
{
    uint32_t max = s->credit < g.cfg.max_frame ? s->credit : g.cfg.max_frame;
    int n = (int)recv(s->fd, (char *)g.tx + WS_HDR + PS_HDR, (int)max, 0);
    if (n > 0) {
        s->credit -= (uint32_t)n;
        send_frame(T_DATA, s->id, (uint16_t)n);
    } else if (n == 0 || !ps_sock_wouldblock()) {
        stream_free(s, true); /* local server closed the connection */
    }
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
        for (int i = 0; i < g.cfg.max_streams && !s; i++) {
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
    int fd = s ? (int)socket(AF_INET, SOCK_STREAM, 0) : -1;
    if (fd < 0) {
        send_close(id, CLOSE_BUSY);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    ps_set_nonblock(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && !ps_sock_inprogress()) {
        ps_closesocket(fd);
        send_close(id, CLOSE_REFUSED);
        return;
    }
    s->id = id;
    s->fd = fd;
    s->credit = n >= 6 ? be32(b + 2) : 4096;
    s->state = rc == 0 ? ST_OPEN : ST_CONNECTING;
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
            if (ps_port_kv_set("token", g.token) != 0) {
                ps_port_logf(PS_LOG_WARN, "could not store the device credential");
            }
        } else if (strncmp(line, "url=", 4) == 0) {
            snprintf(g.url, sizeof(g.url), "%s", line + 4);
            ps_port_logf(PS_LOG_INFO, "online at %s", g.url);
        }
        line = end ? end + 1 : NULL;
    }
}

/* ---- firmware update ---- */

enum { OTA_PROGRESS = 1, OTA_REBOOTING = 2, OTA_FAILED = 3 };

static void ota_status(uint8_t state, const char *text)
{
    uint8_t *b = g.tx + WS_HDR + PS_HDR;
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

    switch (g.rx_op) {
    case OP_BIN:
        break;
    case OP_PING:
        if (n > 125) {
            n = 125;
        }
        memcpy(g.tx + WS_HDR, p, n);
        return ws_send(OP_PONG, n);
    case OP_CLOSE:
        return -1;
    default:
        return 0;
    }
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
        uint8_t *body = g.tx + WS_HDR + PS_HDR, type = 0;
        uint16_t total = 0;

        ps_port_lock();
        while (g.ob_used) {
            uint8_t t = g.ob[g.ob_head];
            uint8_t l = g.ob[(g.ob_head + 1) % g.ob_size];
            if ((total && t != type) || total + l > g.cfg.max_frame) {
                break;
            }
            type = t;
            for (uint16_t i = 0; i < l; i++) {
                body[total + i] = g.ob[(g.ob_head + 2 + i) % g.ob_size];
            }
            total = (uint16_t)(total + l);
            g.ob_head = (uint16_t)((g.ob_head + 2 + l) % g.ob_size);
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

/* ---- connection ---- */

static int conn_wait(uint32_t ms)
{
    if (ps_port_pending(g.conn) > 0) {
        return 1;
    }
    int fd = ps_port_fd(g.conn);
    fd_set r;
    struct timeval tv = { (long)(ms / 1000), (long)(ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_SET(fd, &r);
    return select(fd + 1, &r, NULL, NULL, &tv) > 0;
}

static int hs_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf((char *)g.tx, g.tx_cap, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= g.tx_cap) {
        return -1;
    }
    return ps_port_write(g.conn, g.tx, (size_t)n);
}

static void base64_16(const uint8_t *in, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < 15; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = tbl[v >> 18];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = tbl[v & 63];
    }
    out[o++] = tbl[in[15] >> 2];
    out[o++] = tbl[(in[15] & 3) << 4];
    out[o++] = '=';
    out[o++] = '=';
    out[o] = '\0';
}

/* 0 = upgraded, -2 = credentials refused, -1 = anything else */
static int handshake(void)
{
    uint8_t nonce[16];
    char key[25], info[160], ports[32] = "";
    size_t pl = 0;

    ps_port_random(nonce, sizeof(nonce));
    base64_16(nonce, key);
    ps_port_info(info, sizeof(info));
    for (int i = 0; i < PINSCOPE_MAX_PORTS; i++) {
        if (g.cfg.ports[i] && pl < sizeof(ports) - 7) {
            pl += (size_t)snprintf(ports + pl, sizeof(ports) - pl, pl ? ",%u" : "%u",
                                   (unsigned)g.cfg.ports[i]);
        }
    }

    if (hs_printf("GET /v1/tunnel HTTP/1.1\r\nHost: %s\r\n"
                  "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n"
                  "Sec-WebSocket-Protocol: pinscope.v1\r\n",
                  g.host, key) ||
        hs_printf("Authorization: Bearer %s\r\nX-PS-Device: %s\r\n", g.cfg.project_key,
                  g.device_id) ||
        (g.token[0] && hs_printf("X-PS-Token: %s\r\n", g.token)) ||
        (g.cfg.device_name[0] && hs_printf("X-PS-Name: %s\r\n", g.cfg.device_name)) ||
        hs_printf("X-PS-Info: sdk=" PINSCOPE_VERSION ";streams=%u;frame=%u;ports=%s;ota=%d%s%s\r\n\r\n",
                  (unsigned)g.cfg.max_streams, (unsigned)g.cfg.max_frame, ports,
                  ps_port_ota_supported() ? 1 : 0, info[0] ? ";" : "", info)) {
        return -1;
    }

    char status[13];
    size_t sl = 0;
    uint32_t tail = 0, t0 = ps_port_millis();
    while (tail != 0x0D0A0D0AUL) {
        uint8_t c;
        int n = ps_port_read(g.conn, &c, 1);
        if (n < 0 || (uint32_t)(ps_port_millis() - t0) > HANDSHAKE_MS) {
            return -1;
        }
        if (n == 0) {
            conn_wait(250);
            continue;
        }
        if (sl < sizeof(status) - 1) {
            status[sl++] = (char)c;
        }
        tail = (tail << 8) | c;
    }
    status[sl] = '\0';
    int code = sl >= 12 ? atoi(status + 9) : 0;
    if (code == 101) {
        return 0;
    }
    ps_port_logf(PS_LOG_ERROR, "server answered %d", code);
    return (code == 401 || code == 403) ? -2 : -1;
}

/* A crash recorded before the reboot is delivered first and only forgotten
 * once the server has it. */
static void report_crash(void)
{
    int n = ps_port_crash_read((char *)g.tx + WS_HDR + PS_HDR, g.cfg.max_frame);
    if (n > 0 && send_frame(T_EVENT, 0, (uint16_t)(n > g.cfg.max_frame ? g.cfg.max_frame : n)) == 0) {
        ps_port_crash_clear();
    }
}

static void run_session(void)
{
    int cfd = ps_port_fd(g.conn);
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

        fd_set rf, wf, ef;
        int maxfd = -1;
        bool buffered = !g.pend_len && ps_port_pending(g.conn) > 0;
        FD_ZERO(&rf);
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        if (!g.pend_len) {
            FD_SET(cfd, &rf);
            maxfd = cfd;
        }
        for (int i = 0; i < g.cfg.max_streams; i++) {
            stream_t *s = &g.streams[i];
            if (s->state == ST_FREE) {
                continue;
            }
            if (s->state == ST_CONNECTING) {
                FD_SET(s->fd, &wf);
                FD_SET(s->fd, &ef); /* Winsock reports a failed connect here */
            } else {
                if (s->credit) {
                    FD_SET(s->fd, &rf);
                }
                if (g.pend_s == s) {
                    FD_SET(s->fd, &wf);
                }
            }
            if (s->fd > maxfd) {
                maxfd = s->fd;
            }
        }
        struct timeval tv = { 0, buffered ? 0 : (long)poll_us };
        if (select(maxfd + 1, &rf, &wf, &ef, &tv) < 0) {
            ps_port_sleep(20);
            continue;
        }

        now = ps_port_millis();
        for (int i = 0; i < g.cfg.max_streams; i++) {
            stream_t *s = &g.streams[i];
            if (s->state == ST_CONNECTING) {
                if (FD_ISSET(s->fd, &wf) || FD_ISSET(s->fd, &ef)) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(s->fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
                    if (err == 0 && !FD_ISSET(s->fd, &ef)) {
                        s->state = ST_OPEN;
                        if (g.pend_s == s) {
                            flush_pending();
                        }
                    } else {
                        stream_free(s, true);
                    }
                } else if ((int32_t)(now - s->deadline) >= 0) {
                    stream_free(s, true);
                }
            } else if (s->state == ST_OPEN) {
                if (g.pend_s == s && FD_ISSET(s->fd, &wf)) {
                    flush_pending();
                }
                if (s->state == ST_OPEN && FD_ISSET(s->fd, &rf)) {
                    pump_local(s);
                }
            }
        }
        /* A local server that stopped reading must not freeze the tunnel. */
        if (g.pend_len && (uint32_t)(now - g.pend_since) > STALL_TIMEOUT_MS) {
            stream_free(g.pend_s, true);
        }

        if (!g.pend_len && (buffered || FD_ISSET(cfd, &rf))) {
            int budget = 8;
            do {
                int r = rx_pump();
                if (r < 0 || (r > 0 && dispatch() < 0)) {
                    return;
                }
                if (r == 0) {
                    break;
                }
            } while (!g.pend_len && --budget && ps_port_pending(g.conn) > 0);
        }
    }
}

static void session_cleanup(void)
{
    for (int i = 0; i < g.cfg.max_streams; i++) {
        stream_free(&g.streams[i], false);
    }
    if (g.conn) {
        ps_port_close(g.conn);
        g.conn = NULL;
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
}

static void task_main(void *arg)
{
    uint32_t backoff = BACKOFF_MIN_MS;
    (void)arg;

    while (!g.stop) {
        uint32_t started = ps_port_millis(), jitter;
        int rc;

        set_state(PINSCOPE_CONNECTING);
        rc = ps_port_connect(&g.conn, g.host, g.port, g.tls, g.cfg.ca_cert_pem);
        if (rc == 0) {
            rc = handshake();
        } else {
            g.conn = NULL;
            rc = -1;
        }
        if (rc == 0) {
            set_state(PINSCOPE_ONLINE);
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
        }
    }
    set_state(PINSCOPE_STOPPED);
    g.running = false;
}

/* ---- public API ---- */

static int parse_url(const char *u)
{
    if (strncmp(u, "wss://", 6) == 0 || strncmp(u, "https://", 8) == 0) {
        g.tls = true;
        g.port = 443;
    } else if (strncmp(u, "ws://", 5) == 0 || strncmp(u, "http://", 7) == 0) {
        g.tls = false;
        g.port = 80;
    } else {
        return -1;
    }
    u = strstr(u, "://") + 3;
    size_t n = strcspn(u, ":/");
    if (n == 0 || n >= sizeof(g.host)) {
        return -1;
    }
    memcpy(g.host, u, n);
    g.host[n] = '\0';
    if (u[n] == ':') {
        g.port = (uint16_t)atoi(u + n + 1);
    }
    return g.port ? 0 : -1;
}

static const char *pick(const char *value, const char *fallback)
{
    return (value && value[0]) ? value : fallback;
}

static bool opt(pinscope_opt_t o, bool fallback)
{
    return o == PINSCOPE_DEFAULT ? fallback : o == PINSCOPE_ON;
}

/* Set only by the simulator and the test suite, which run against a server
 * on the same machine. */
const char *ps_test_server_url;

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
    if (!c->max_streams) {
        c->max_streams = CONFIG_PINSCOPE_MAX_STREAMS;
    }
    if (!c->max_frame) {
        c->max_frame = CONFIG_PINSCOPE_MAX_FRAME;
    }
    c->max_frame = c->max_frame < 512 ? 512 : c->max_frame > 4096 ? 4096 : c->max_frame;
    if (!c->outbox_size) {
        c->outbox_size = CONFIG_PINSCOPE_OUTBOX_SIZE;
    }
    if (!c->task_stack) {
        c->task_stack = CONFIG_PINSCOPE_TASK_STACK;
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
        return -1;
    }

    g.rx_cap = (uint16_t)(PS_HDR + c->max_frame);
    g.tx_cap = (uint16_t)(WS_HDR + PS_HDR + c->max_frame);
    g.rx = malloc((size_t)g.rx_cap + 1);
    g.tx = malloc(g.tx_cap);
    g.streams = calloc(c->max_streams, sizeof(stream_t));
    uint8_t *ob = c->outbox_size >= 64 ? malloc(c->outbox_size) : NULL;
    if (!g.rx || !g.tx || !g.streams || (c->outbox_size >= 64 && !ob)) {
        free(g.rx);
        free(g.tx);
        free(g.streams);
        free(ob);
        return -1;
    }
    g.ob_size = ob ? c->outbox_size : 0;
    g.ob = ob;

    ps_port_device_id(g.device_id, sizeof(g.device_id));
    if (ps_port_kv_get("token", g.token, sizeof(g.token)) != 0) {
        g.token[0] = '\0';
    }
    ps_port_log_capture(logs);

    g.running = true;
    set_state(PINSCOPE_CONNECTING);
    if (ps_port_task_start(task_main, c->task_stack, c->task_priority) != 0) {
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
        return; /* task is stuck in a connect; keep its buffers alive */
    }
    ps_port_log_capture(false);
    ps_port_lock();
    uint8_t *ob = g.ob;
    g.ob = NULL;
    ps_port_unlock();
    free(ob);
    free(g.rx);
    free(g.tx);
    free(g.streams);
    g.rx = g.tx = NULL;
    g.streams = NULL;
}

pinscope_state_t pinscope_state(void) { return (pinscope_state_t)g.state; }

const char *pinscope_url(void) { return g.url; }

void pinscope_kick(void) { g.kick = true; }
