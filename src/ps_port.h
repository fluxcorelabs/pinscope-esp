/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 FluxCore Labs. See LICENSE: for use with the Pinscope service only. */
/*
 * Platform layer. The core (ps_core.c) only needs what is declared here plus
 * BSD sockets, for the connection to the service and the loopback side of
 * each stream.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define ps_closesocket(fd)   closesocket(fd)
#define ps_sock_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
#define ps_sock_inprogress() (WSAGetLastError() == WSAEWOULDBLOCK)
static inline int ps_set_nonblock(int fd)
{
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on);
}
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define ps_closesocket(fd)   close(fd)
#define ps_sock_wouldblock() (errno == EAGAIN || errno == EWOULDBLOCK)
#define ps_sock_inprogress() (errno == EINPROGRESS)
static inline int ps_set_nonblock(int fd)
{
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}
#endif

#if defined(__GNUC__)
#define PS_NOINLINE __attribute__((noinline))
#else
#define PS_NOINLINE
#endif

#ifdef MSG_NOSIGNAL
#define PS_SEND_FLAGS MSG_NOSIGNAL
#else
#define PS_SEND_FLAGS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum { PS_LOG_ERROR, PS_LOG_WARN, PS_LOG_INFO };

/* For the simulator and tests only: a server on the same machine. */
extern const char *ps_test_server_url;

/* Stack of the Pinscope task, in bytes. */
#ifndef PS_TASK_STACK
#define PS_TASK_STACK 3072
#endif

int      ps_port_init(void);
uint32_t ps_port_millis(void);
void     ps_port_sleep(uint32_t ms);
void     ps_port_random(void *buf, size_t len);
void     ps_port_lock(void);
void     ps_port_unlock(void);
/* From the core: queue log text while already holding ps_port_lock(). */
void     ps_log_locked(const char *text, size_t len);
int      ps_port_task_start(void (*fn)(void *), uint32_t stack, uint8_t priority);
void     ps_port_logf(int level, const char *fmt, ...);

/* Stable hardware id, [A-Za-z0-9_-]{4,40}. */
void ps_port_device_id(char *buf, size_t len);
/* "key=value;key=value" describing chip / firmware, may be empty. */
void ps_port_info(char *buf, size_t len);

/* Small persistent string store. Return 0 on success. */
int ps_port_kv_get(const char *key, char *buf, size_t len);
int ps_port_kv_set(const char *key, const char *value);

void ps_port_log_capture(bool on);
/* Crash captured before the last reboot, as text: a "crash" line followed by
 * key=value lines. Returns its length, 0 when there is none. Cleared once it
 * has been sent. */
int  ps_port_crash_read(char *buf, size_t len);
void ps_port_crash_clear(void);
/* Over-the-air update into the spare application partition. begin/write/end
 * return 0 on success; end verifies the image and makes it the next to boot. */
bool ps_port_ota_supported(void);
int  ps_port_ota_begin(uint32_t size);
int  ps_port_ota_write(const void *data, size_t len);
int  ps_port_ota_end(void);
void ps_port_ota_abort(void);
/* Tell the bootloader the running image works, cancelling a pending rollback. */
void ps_port_ota_confirm(void);
/* True while a freshly updated image runs on trial: the bootloader returns to
 * the previous one at the next reset unless it is confirmed first. */
bool ps_port_ota_on_trial(void);
void ps_port_restart(void);

/* PINSCOPE_M_* bits selected at build time. */
uint16_t ps_port_default_metrics(void);
/* Report the selected PINSCOPE_M_* groups through pinscope_metric(). */
void ps_port_sys_metrics(uint16_t mask);

/* False while the device has no network address, so that connecting is not
 * even tried. */
bool ps_port_network_ready(void);
/* IPv4 address of `host` (a name or dotted quad), network byte order.
 * `scratch` (at least 512 bytes) is free for the port to use meanwhile.
 * 0 on success. */
int ps_port_resolve(const char *host, uint8_t *scratch, size_t len, uint32_t *addr);

#ifdef __cplusplus
}
#endif
