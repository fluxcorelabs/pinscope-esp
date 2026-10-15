/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 Filip Code. See LICENSE: for use with the Pinscope service only. */
/* Pinscope platform layer for ESP-IDF (also used by Arduino-ESP32). */
#ifdef ESP_PLATFORM

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_debug_helpers.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
/* ESP-IDF 4.4 (Arduino-ESP32 2.x, PlatformIO's default) keeps these elsewhere. */
#if __has_include("esp_app_desc.h")
#include "esp_app_desc.h"
#else
#define esp_app_get_description esp_ota_get_app_description
#define esp_app_get_elf_sha256 esp_ota_get_app_elf_sha256
#endif
#if __has_include("esp_cpu.h")
#include "esp_cpu.h"
#endif
#if __has_include("esp_memory_utils.h")
#include "esp_memory_utils.h"
#else
#include "soc/soc_memory_layout.h"
#endif
#include "esp_private/panic_internal.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/pbuf.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
#include "lwip/priv/tcpip_priv.h" /* tcpip_api_call */
#endif
#include "nvs.h"
#include "sdkconfig.h"

#include "soc/soc_caps.h"
/* The task iterator moved between ESP-IDF 5 and 6. */
#if __has_include("freertos/freertos_debug.h")
#include "freertos/freertos_debug.h"
#define PS_HAVE_TASK_ITERATOR 1
#elif __has_include("esp_private/freertos_debug.h")
#include "esp_private/freertos_debug.h"
#define PS_HAVE_TASK_ITERATOR 1
#endif
#if SOC_TEMP_SENSOR_SUPPORTED && __has_include("driver/temperature_sensor.h") && \
    (defined(CONFIG_PINSCOPE_METRIC_TEMPERATURE) || !defined(CONFIG_PINSCOPE_KCONFIG))
#include "driver/temperature_sensor.h"
#define PS_HAVE_TEMP_SENSOR 1
#endif
#ifdef CONFIG_PINSCOPE_CRASH_CAPTURE /* the panic handler's frame types */
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "esp_cpu_utils.h"
#include "xtensa_context.h"
#else
#include "riscv/rvruntime-frames.h"
#endif
#endif

#include "pinscope.h"
#include "ps_port.h"

#define NVS_NAMESPACE "pinscope"

static const char *TAG = "pinscope";

static SemaphoreHandle_t s_lock; /* the outbox, and s_log_line with it */
static vprintf_like_t    s_prev_vprintf;
static bool              s_capturing;
static char              s_log_line[192];

static uint32_t s_boot_id;
static uint32_t s_wifi_disconnects;
static uint8_t  s_wifi_last_reason;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    pinscope_kick();
}

static void on_wifi_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    s_wifi_disconnects++;
    s_wifi_last_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
}

static void watch_panics(void);

int ps_port_init(void)
{
    if (s_lock) {
        return 0;
    }
    watch_panics();
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return -1;
    }
    /* Fails harmlessly when the app has no default event loop yet. */
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_disconnect, NULL);
    s_boot_id = esp_random();
    return 0;
}

uint32_t ps_port_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void ps_port_sleep(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void ps_port_random(void *buf, size_t len) { esp_fill_random(buf, len); }

void ps_port_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }

void ps_port_unlock(void) { xSemaphoreGive(s_lock); }

static void (*s_task_fn)(void *);

static void task_trampoline(void *arg)
{
    s_task_fn(arg);
    vTaskDelete(NULL);
}

int ps_port_task_start(void (*fn)(void *), uint32_t stack, uint8_t priority)
{
    s_task_fn = fn;
    return xTaskCreate(task_trampoline, "pinscope", stack, NULL, priority, NULL) == pdPASS ? 0 : -1;
}

void ps_port_logf(int level, const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (level == PS_LOG_ERROR) {
        ESP_LOGE(TAG, "%s", msg);
    } else if (level == PS_LOG_WARN) {
        ESP_LOGW(TAG, "%s", msg);
    } else {
        ESP_LOGI(TAG, "%s", msg);
    }
}

void ps_port_device_id(char *buf, size_t len)
{
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    snprintf(buf, len, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_EXT:       return "external";
    default:                return "other";
    }
}

void ps_port_info(char *buf, size_t len)
{
#if defined(CONFIG_PINSCOPE_DEVICE_INFO) || !defined(CONFIG_PINSCOPE_KCONFIG)
    const esp_app_desc_t *app = esp_app_get_description();
    char elf[17] = ""; /* identifies the exact build a backtrace belongs to */
    esp_app_get_elf_sha256(elf, sizeof(elf));
    /* boot changes on every restart, which is how the server counts them */
    snprintf(buf, len, "chip=%s;fw=%.31s;idf=%.15s;reset=%s;elf=%s;boot=%08x", CONFIG_IDF_TARGET,
             app->version, esp_get_idf_version(), reset_reason(), elf, (unsigned)s_boot_id);
#else
    buf[0] = '\0';
#endif
}

int ps_port_kv_get(const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

int ps_port_kv_set(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

static int log_vprintf(const char *fmt, va_list ap)
{
    if (s_capturing && !xPortInIsrContext() &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(s_log_line, sizeof(s_log_line), fmt, copy);
        va_end(copy);
        if (n > 0) {
            ps_log_locked(s_log_line, n < (int)sizeof(s_log_line) ? (size_t)n : sizeof(s_log_line) - 1);
        }
        xSemaphoreGive(s_lock);
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
}

void ps_port_log_capture(bool on)
{
    /* The hook stays installed once set so a later vprintf override by the app
     * is never clobbered on stop; capture is just switched off. */
    if (on && !s_prev_vprintf) {
        s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
    }
    s_capturing = on;
}

/* ---- crash capture ----
 *
 * The panic handler is wrapped at link time. It only copies the reason and the
 * call chain into RTC memory, which survives the reboot that follows; the
 * record is sent on the next connection.
 */

#define CRASH_MAGIC  0x50534352u /* "PSCR" */
#define CRASH_MAGIC2 0x50534332u /* "PSC2": the fields after `task` are set */
#define CRASH_FRAMES 24

typedef struct {
    uint32_t magic;
    uint32_t pc;
    uint32_t addr[CRASH_FRAMES];
    uint8_t  count;
    uint8_t  approx; /* addr[] is a stack scan, not an exact unwind */
    char     reason[40];
    char     detail[72];
    char     task[16];
    /* Added in 0.2.1, at the end so that older firmware still reads the rest.
     * The image that crashed is not always the one that reports it: after a
     * bad update the bootloader goes back to the previous image. */
    uint32_t magic2;
    char     elf[17]; /* start of the crashed image's ELF hash */
} crash_record_t;

static RTC_NOINIT_ATTR crash_record_t s_crash;

/* This image's ELF hash, read once at start-up: the panic handler may run
 * with the flash cache off and must not call into flash for it. */
static char s_elf[17];

static void __attribute__((constructor)) remember_elf(void)
{
    esp_app_get_elf_sha256(s_elf, sizeof(s_elf));
}

#ifdef CONFIG_PINSCOPE_CRASH_CAPTURE

static void copy_text(char *dst, size_t size, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i < size - 1; i++) {
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? src[i] : ' ';
    }
    dst[i] = '\0';
}

void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    crash_record_t *r = &s_crash;
    r->magic = 0;
    r->magic2 = 0;
    r->count = 0;
    r->approx = 0;
    r->pc = (uint32_t)(uintptr_t)info->addr;
    copy_text(r->reason, sizeof(r->reason), info->reason);
    copy_text(r->detail, sizeof(r->detail), info->description);
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    copy_text(r->task, sizeof(r->task), task ? pcTaskGetName(task) : "");

    if (info->frame) {
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        const XtExcFrame *f = info->frame;
        esp_backtrace_frame_t bt = { .pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = f };
        r->addr[r->count++] = esp_cpu_process_stack_pc(bt.pc);
        while (r->count < CRASH_FRAMES && bt.next_pc != 0 && esp_backtrace_get_next_frame(&bt)) {
            r->addr[r->count++] = esp_cpu_process_stack_pc(bt.pc);
        }
#else
        /* No frame pointers on RISC-V: keep pc, ra and every stack word that
         * points into code. Some of them are stale, hence "approx". */
        const RvExcFrame *f = info->frame;
        const uint32_t *sp = (const uint32_t *)f->sp;
        r->approx = 1;
        r->addr[r->count++] = f->mepc;
        r->addr[r->count++] = f->ra;
        for (int i = 0; i < 256 && r->count < CRASH_FRAMES && esp_ptr_in_dram(&sp[i]); i++) {
            if (esp_ptr_executable((void *)sp[i])) {
                r->addr[r->count++] = sp[i];
            }
        }
#endif
    }
    memcpy(r->elf, s_elf, sizeof(r->elf));
    r->magic2 = CRASH_MAGIC2;
    r->magic = CRASH_MAGIC;
    __real_esp_panic_handler(info);
}

#endif /* CONFIG_PINSCOPE_CRASH_CAPTURE */

/* Arduino-ESP32 3.x wraps the panic handler itself and passes the crash on
 * to one registered handler: Pinscope records it there, and then calls the
 * handler the application had set, if any. */
#if !defined(CONFIG_PINSCOPE_CRASH_CAPTURE) && defined(ARDUINO_ARCH_ESP32) && __has_include("esp_arduino_version.h")
#include "esp_arduino_version.h"
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include "esp32-hal.h"
#define PS_ARDUINO_PANIC 1
static arduino_panic_handler_t s_app_panic;
static void *s_app_panic_arg;

static void on_arduino_panic(arduino_panic_info_t *info, void *arg)
{
    crash_record_t *r = &s_crash;
    memset(r, 0, sizeof(*r));
    r->pc = (uint32_t)(uintptr_t)info->pc;
    snprintf(r->reason, sizeof(r->reason), "%s", info->reason ? info->reason : "panic");
    for (char *c = r->reason; *c; c++) {
        *c = (*c >= 0x20 && *c < 0x7f) ? *c : ' ';
    }
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    snprintf(r->task, sizeof(r->task), "%s", task ? pcTaskGetName(task) : "");
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    unsigned first = 0;
#else
    unsigned first = 1; /* the stack pointer, then every stack word that points into code */
    r->approx = 1;
    r->addr[r->count++] = r->pc;
#endif
    for (unsigned i = first; i < info->backtrace_len && r->count < CRASH_FRAMES; i++) {
        r->addr[r->count++] = info->backtrace[i];
    }
    memcpy(r->elf, s_elf, sizeof(r->elf));
    r->magic2 = CRASH_MAGIC2;
    r->magic = CRASH_MAGIC;
    if (s_app_panic) {
        s_app_panic(info, s_app_panic_arg);
    }
}

static void watch_panics(void)
{
    if (get_arduino_panic_handler() != on_arduino_panic) {
        s_app_panic = get_arduino_panic_handler();
        s_app_panic_arg = get_arduino_panic_handler_arg();
        set_arduino_panic_handler(on_arduino_panic, NULL);
    }
}
#endif
#endif
#ifndef PS_ARDUINO_PANIC
static void watch_panics(void) {}
#endif

int ps_port_crash_read(char *buf, size_t len)
{
    crash_record_t *r = &s_crash;
    if (r->magic != CRASH_MAGIC || r->count > CRASH_FRAMES) {
        return 0;
    }
    r->reason[sizeof(r->reason) - 1] = r->detail[sizeof(r->detail) - 1] = '\0';
    r->task[sizeof(r->task) - 1] = '\0';
    int n = snprintf(buf, len, "crash\nreason=%s\ndetail=%s\ntask=%s\npc=0x%08x\napprox=%d\n",
                     r->reason, r->detail, r->task, (unsigned)r->pc, (int)r->approx);
    if (r->magic2 == CRASH_MAGIC2 && n > 0 && (size_t)n + sizeof(r->elf) + 8 < len) {
        r->elf[sizeof(r->elf) - 1] = '\0';
        if (strspn(r->elf, "0123456789abcdef") == strlen(r->elf) && r->elf[0]) {
            n += snprintf(buf + n, len - (size_t)n, "elf=%s\n", r->elf);
        }
    }
    if (n > 0 && (size_t)n + 4 < len) {
        n += snprintf(buf + n, len - (size_t)n, "bt=");
    }
    for (int i = 0; i < r->count && n > 0 && (size_t)n + 12 < len; i++) {
        n += snprintf(buf + n, len - (size_t)n, i ? " 0x%08x" : "0x%08x", (unsigned)r->addr[i]);
    }
    return (n > 0 && (size_t)n < len) ? n : 0;
}

void ps_port_crash_clear(void) { s_crash.magic = 0; }

/* ---- firmware update: the standard two-slot scheme of esp_ota ---- */

#if defined(CONFIG_PINSCOPE_OTA) || !defined(CONFIG_PINSCOPE_KCONFIG)

static esp_ota_handle_t       s_ota;
static const esp_partition_t *s_ota_slot;

bool ps_port_ota_supported(void) { return esp_ota_get_next_update_partition(NULL) != NULL; }

int ps_port_ota_begin(uint32_t size)
{
    s_ota_slot = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_slot || size > s_ota_slot->size) {
        return -1;
    }
    /* Sequential mode erases as it writes instead of stalling on the whole slot. */
    return esp_ota_begin(s_ota_slot, OTA_WITH_SEQUENTIAL_WRITES, &s_ota) == ESP_OK ? 0 : -1;
}

int ps_port_ota_write(const void *data, size_t len)
{
    return esp_ota_write(s_ota, data, len) == ESP_OK ? 0 : -1;
}

int ps_port_ota_end(void)
{
    /* esp_ota_end checks the image (and its signature when secure boot is on). */
    if (esp_ota_end(s_ota) != ESP_OK) {
        return -1;
    }
    return esp_ota_set_boot_partition(s_ota_slot) == ESP_OK ? 0 : -1;
}

void ps_port_ota_abort(void) { esp_ota_abort(s_ota); }

bool ps_port_ota_on_trial(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void ps_port_ota_confirm(void)
{
    if (ps_port_ota_on_trial()) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed, rollback cancelled");
    }
}

#else

bool ps_port_ota_supported(void) { return false; }
int  ps_port_ota_begin(uint32_t size) { return -1; }
int  ps_port_ota_write(const void *data, size_t len) { return -1; }
int  ps_port_ota_end(void) { return -1; }
void ps_port_ota_abort(void) {}
void ps_port_ota_confirm(void) {}
bool ps_port_ota_on_trial(void) { return false; }

#endif

void ps_port_restart(void) { esp_restart(); }

/* ---- built-in metrics ---- */

uint16_t ps_port_default_metrics(void)
{
    uint16_t mask = 0;
#ifndef CONFIG_PINSCOPE_KCONFIG /* e.g. Arduino: everything that needs no driver */
    mask = PINSCOPE_M_HEAP | PINSCOPE_M_UPTIME | PINSCOPE_M_RSSI | PINSCOPE_M_WIFI | PINSCOPE_M_STACK |
           PINSCOPE_M_TASKS | PINSCOPE_M_CPU;
#endif
#ifdef CONFIG_PINSCOPE_METRIC_HEAP
    mask |= PINSCOPE_M_HEAP;
#endif
#ifdef CONFIG_PINSCOPE_METRIC_UPTIME
    mask |= PINSCOPE_M_UPTIME;
#endif
#ifdef CONFIG_PINSCOPE_METRIC_RSSI
    mask |= PINSCOPE_M_RSSI;
#endif
#ifdef CONFIG_PINSCOPE_METRIC_WIFI_DISCONNECTS
    mask |= PINSCOPE_M_WIFI;
#endif
#ifdef CONFIG_PINSCOPE_METRIC_TASK_STACK
    mask |= PINSCOPE_M_STACK;
#endif
#ifdef CONFIG_PINSCOPE_METRIC_TASK_STACK_EACH
    mask |= PINSCOPE_M_TASKS;
#endif
#ifdef CONFIG_PINSCOPE_METRIC_TASK_CPU
    mask |= PINSCOPE_M_CPU;
#endif
#ifdef CONFIG_PINSCOPE_METRIC_TEMPERATURE
    mask |= PINSCOPE_M_TEMP;
#endif
    return mask;
}

/* Per task: free stack ("stack/<task>") and, when FreeRTOS keeps run-time
 * statistics, the share of CPU time since the previous report ("cpu/<task>").
 * Overall: "stack_min" and "cpu_load". */
#define TASKS_MAX 16

typedef struct {
    char     name[configMAX_TASK_NAME_LEN];
    uint32_t free_bytes;
    float    cpu; /* percent of all CPU time, < 0 when unknown */
} task_info_t;

#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
#define PS_HAVE_CPU_STATS 1
static struct {
    UBaseType_t number;
    uint32_t    counter;
} s_cpu_prev[TASKS_MAX];
static int s_cpu_prev_count;
#endif

static void report_tasks(uint16_t mask)
{
    static uint32_t warned = UINT32_MAX;
    task_info_t *tasks = calloc(TASKS_MAX, sizeof(task_info_t));
    int count = 0;
    if (!tasks) {
        return;
    }

#if configUSE_TRACE_FACILITY
    UBaseType_t total = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *status = malloc(total * sizeof(TaskStatus_t));
    if (status) {
        total = uxTaskGetSystemState(status, total, NULL);
        if (total > TASKS_MAX) {
            total = TASKS_MAX;
        }
#if PS_HAVE_CPU_STATS
        /* Run time since the last report, per task and summed over all tasks. */
        uint32_t delta[TASKS_MAX], all = 0;
        for (UBaseType_t i = 0; i < total; i++) {
            delta[i] = 0;
            for (int j = 0; j < s_cpu_prev_count; j++) {
                if (s_cpu_prev[j].number == status[i].xTaskNumber) {
                    delta[i] = (uint32_t)status[i].ulRunTimeCounter - s_cpu_prev[j].counter;
                }
            }
            all += delta[i];
        }
#endif
        for (UBaseType_t i = 0; i < total; i++, count++) {
            strlcpy(tasks[count].name, status[i].pcTaskName, sizeof(tasks[count].name));
            tasks[count].free_bytes = status[i].usStackHighWaterMark;
            tasks[count].cpu = -1;
#if PS_HAVE_CPU_STATS
            if (all > 0) {
                tasks[count].cpu = (float)delta[i] * 100.0f / (float)all;
            }
#endif
        }
#if PS_HAVE_CPU_STATS
        for (UBaseType_t i = 0; i < total; i++) {
            s_cpu_prev[i].number = status[i].xTaskNumber;
            s_cpu_prev[i].counter = (uint32_t)status[i].ulRunTimeCounter;
        }
        s_cpu_prev_count = (int)total;
#endif
        free(status);
    }
#elif PS_HAVE_TASK_ITERATOR && CONFIG_FREERTOS_UNICORE
    /* The iterator takes no locks; with one core a suspended scheduler is enough. */
    TaskIterator_t it = { 0 };
    vTaskSuspendAll();
    while (count < TASKS_MAX && xTaskGetNext(&it) != -1) {
        strlcpy(tasks[count].name, pcTaskGetName(it.pxTaskHandle), sizeof(tasks[count].name));
        tasks[count].cpu = -1;
        tasks[count++].free_bytes = uxTaskGetStackHighWaterMark(it.pxTaskHandle);
    }
    xTaskResumeAll();
#endif
    if (count == 0) { /* no safe way to walk the task list: own task only */
        strlcpy(tasks[0].name, pcTaskGetName(NULL), sizeof(tasks[0].name));
        tasks[0].cpu = -1;
        tasks[count++].free_bytes = uxTaskGetStackHighWaterMark(NULL);
    }

    const task_info_t *least = &tasks[0];
    float idle = -1;
    char metric[8 + configMAX_TASK_NAME_LEN];
    for (int i = 0; i < count; i++) {
        if (tasks[i].free_bytes < least->free_bytes) {
            least = &tasks[i];
        }
        if (mask & PINSCOPE_M_TASKS) {
            snprintf(metric, sizeof(metric), "stack/%s", tasks[i].name);
            pinscope_metric(metric, (double)tasks[i].free_bytes);
        }
        if (tasks[i].cpu >= 0 && (mask & PINSCOPE_M_CPU)) {
            snprintf(metric, sizeof(metric), "cpu/%s", tasks[i].name);
            pinscope_metric(metric, (double)tasks[i].cpu);
            if (strncmp(tasks[i].name, "IDLE", 4) == 0) {
                idle = (idle < 0 ? 0 : idle) + tasks[i].cpu;
            }
        }
    }
    if (idle >= 0) {
        pinscope_metric("cpu_load", (double)(100.0f - idle));
    }
    if (mask & PINSCOPE_M_STACK) {
        pinscope_metric("stack_min", (double)least->free_bytes);
    }
#if CONFIG_PINSCOPE_STACK_WARNING
    if (least->free_bytes < CONFIG_PINSCOPE_STACK_WARNING && least->free_bytes < warned) {
        warned = least->free_bytes;
        ESP_LOGW(TAG, "task '%s' has only %u bytes of stack left", least->name, (unsigned)least->free_bytes);
    }
#else
    (void)warned;
#endif
    free(tasks);
}

#if PS_HAVE_TEMP_SENSOR
static void report_temperature(void)
{
    static temperature_sensor_handle_t sensor;
    static bool unavailable;
    float celsius;

    if (!sensor && !unavailable) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        unavailable = temperature_sensor_install(&cfg, &sensor) != ESP_OK; /* app owns it */
    }
    if (unavailable || temperature_sensor_enable(sensor) != ESP_OK) {
        return;
    }
    if (temperature_sensor_get_celsius(sensor, &celsius) == ESP_OK) {
        pinscope_metric("chip_temp_c", (double)celsius);
    }
    temperature_sensor_disable(sensor);
}
#endif

void ps_port_sys_metrics(uint16_t mask)
{
    wifi_ap_record_t ap;
    if (mask & PINSCOPE_M_HEAP) {
        pinscope_metric("heap_free", (double)esp_get_free_heap_size());
        pinscope_metric("heap_min", (double)esp_get_minimum_free_heap_size());
        pinscope_metric("heap_largest", (double)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }
    if (mask & PINSCOPE_M_UPTIME) {
        pinscope_metric("uptime_s", (double)(esp_timer_get_time() / 1000000));
    }
    if ((mask & PINSCOPE_M_RSSI) && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        pinscope_metric("rssi", (double)ap.rssi);
    }
    if (mask & PINSCOPE_M_WIFI) {
        pinscope_metric("wifi_disconnects", (double)s_wifi_disconnects);
        if (s_wifi_disconnects) {
            pinscope_metric("wifi_last_reason", (double)s_wifi_last_reason);
        }
    }
    if (mask & (PINSCOPE_M_STACK | PINSCOPE_M_TASKS | PINSCOPE_M_CPU)) {
        report_tasks(mask);
    }
#if PS_HAVE_TEMP_SENSOR
    if (mask & PINSCOPE_M_TEMP) {
        report_temperature();
    }
#endif
}

/* ---- connections to local servers ----
 *
 * These use lwIP's callback API, not sockets. A socket keeps at most
 * CONFIG_LWIP_TCP_RECVMBOX_SIZE received packets (6 unless the application
 * raises it) and lwIP drops what does not fit; a web server that answers in
 * small pieces overruns that long before the TCP window closes, and each time
 * the transfer waits 1.5 s for a retransmission. Here every packet is taken
 * as it arrives and lwIP is told how much has been read, so the window, in
 * bytes, is the only limit, whatever the application's configuration.
 *
 * Everything that touches a connection runs in lwIP's own thread
 * (tcpip_callback_wait); the callbacks run there too, so nothing needs a
 * lock. News for the Pinscope task is a flag per connection; when the task
 * is waiting in select(), a one-byte datagram to a loopback socket wakes it.
 */

#define LOCAL_MAX      4
/* Six segments of this size fill a default lwIP window (5760 bytes), and six
 * is what the server's socket may hold: data to it is sent in pieces at
 * least this large, or when nothing more is coming. */
#define LOCAL_FLUSH_AT 960

enum { L_FREE = 0, L_CONNECTING, L_OPEN, L_FAILED };

typedef struct {
    struct tcp_pcb  *pcb; /* NULL once lwIP has dropped the connection */
    struct pbuf     *rx;  /* received and not read yet */
    volatile uint8_t state;
    volatile bool    eof;
    volatile bool    news;      /* something for the task to look at */
    volatile uint16_t want;     /* received data is news once there is this much */
    bool             want_sent; /* a write did not fit: tell the task when room appears */
} local_t;

enum { LC_OPEN, LC_READ, LC_WRITE, LC_FLUSH, LC_CLOSE };

typedef struct {
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    struct tcpip_api_call_data call; /* first: lwIP hands this pointer back */
#endif
    int      op, h, ret;
    uint16_t port;
    void    *buf;
    size_t   len;
} local_call_t;

static local_t         s_local[LOCAL_MAX];
static struct udp_pcb *s_wake_tx;
static int             s_wake_fd = -1;
static uint16_t        s_wake_port;
static volatile bool   s_wake_sent, s_unflushed;
static volatile bool   s_sleeping; /* the task is in, or about to enter, select() */

static void local_wake(local_t *l)
{
    struct pbuf *p;
    ip_addr_t lo;
    l->news = true;
    /* A busy task finds the flag by itself (ps_local_ready). */
    if (!s_sleeping || s_wake_sent || !s_wake_tx || !(p = pbuf_alloc(PBUF_TRANSPORT, 1, PBUF_RAM))) {
        return;
    }
    IP_ADDR4(&lo, 127, 0, 0, 1);
    *(uint8_t *)p->payload = 0;
    if (udp_sendto(s_wake_tx, p, &lo, s_wake_port) == ERR_OK) {
        s_wake_sent = true;
    }
    pbuf_free(p);
}

static err_t local_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    local_t *l = arg;
    if (l) {
        l->state = L_OPEN;
        local_wake(l);
    }
    return ERR_OK;
}

static err_t local_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    local_t *l = arg;
    if (!l) { /* closed on our side: discard */
        if (p) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }
        return ERR_OK;
    }
    if (!p) {
        l->eof = true;
    } else if (l->rx) {
        pbuf_cat(l->rx, p);
    } else {
        l->rx = p;
    }
    if (!p || l->rx->tot_len >= l->want) {
        local_wake(l);
    }
    return ERR_OK;
}

static void local_err(void *arg, err_t err)
{
    local_t *l = arg;
    if (l) {
        l->pcb = NULL; /* lwIP has freed it */
        if (l->state == L_CONNECTING) {
            l->state = L_FAILED;
        }
        l->eof = true;
        local_wake(l);
    }
}

static err_t local_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    local_t *l = arg;
    if (l && l->want_sent) {
        l->want_sent = false;
        local_wake(l); /* room for what was held back */
    }
    return ERR_OK;
}

static void local_do(void *ctx)
{
    local_call_t *c = ctx;
    local_t *l = (c->h >= 0 && c->h < LOCAL_MAX) ? &s_local[c->h] : NULL;
    ip_addr_t lo;

    c->ret = -1;
    switch (c->op) {
    case LC_OPEN:
        for (int i = 0; i < LOCAL_MAX && c->ret < 0; i++) {
            if (s_local[i].state == L_FREE) {
                c->ret = i;
            }
        }
        if (c->ret < 0) {
            break;
        }
        l = &s_local[c->ret];
        memset(l, 0, sizeof(*l));
        l->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!l->pcb) {
            c->ret = -1;
            break;
        }
        if (!s_wake_tx) {
            s_wake_tx = udp_new();
        }
        l->state = L_CONNECTING;
        tcp_arg(l->pcb, l);
        tcp_recv(l->pcb, local_recv);
        tcp_err(l->pcb, local_err);
        tcp_sent(l->pcb, local_sent);
        tcp_nagle_disable(l->pcb); /* LC_WRITE decides when to send */
        IP_ADDR4(&lo, 127, 0, 0, 1);
        if (tcp_connect(l->pcb, &lo, c->port, local_connected) != ERR_OK) {
            tcp_arg(l->pcb, NULL);
            tcp_abort(l->pcb);
            memset(l, 0, sizeof(*l));
            c->ret = -1;
        }
        break;

    case LC_READ:
        if (!l || l->state == L_FREE) {
            break;
        }
        c->ret = 0;
        if (l->rx) {
            u16_t n = pbuf_copy_partial(l->rx, c->buf, (u16_t)(c->len > 0xffff ? 0xffff : c->len), 0);
            l->rx = pbuf_free_header(l->rx, n);
            if (l->pcb) {
                tcp_recved(l->pcb, n); /* opens the window by what was read */
            }
            c->ret = n;
        }
        if (c->ret == 0 && (l->eof || !l->pcb)) {
            c->ret = -1;
        }
        break;

    case LC_WRITE:
        if (!l || !l->pcb || l->state != L_OPEN) {
            break;
        } else {
            struct tcp_pcb *pcb = l->pcb;
            size_t n = c->len < tcp_sndbuf(pcb) ? c->len : tcp_sndbuf(pcb);
            err_t err = n ? tcp_write(pcb, c->buf, (u16_t)n, TCP_WRITE_FLAG_COPY) : ERR_MEM;
            if (err == ERR_MEM) {
                l->want_sent = true; /* later: local_sent() wakes the task */
                c->ret = 0;
            } else if (err == ERR_OK) {
                if ((u32_t)(pcb->snd_lbb - pcb->snd_nxt) >= LOCAL_FLUSH_AT || pcb->unacked == NULL) {
                    tcp_output(pcb);
                } else {
                    s_unflushed = true;
                }
                c->ret = (int)n;
            }
        }
        break;

    case LC_FLUSH:
        for (int i = 0; i < LOCAL_MAX; i++) {
            if (s_local[i].state == L_OPEN && s_local[i].pcb) {
                tcp_output(s_local[i].pcb);
            }
        }
        c->ret = 0;
        break;

    case LC_CLOSE:
        if (!l || l->state == L_FREE) {
            break;
        }
        if (l->pcb) {
            tcp_arg(l->pcb, NULL);
            tcp_recv(l->pcb, local_recv); /* with no argument it discards */
            tcp_err(l->pcb, NULL);
            tcp_sent(l->pcb, NULL);
            if (tcp_close(l->pcb) != ERR_OK) {
                tcp_abort(l->pcb);
            }
        }
        if (l->rx) {
            pbuf_free(l->rx);
        }
        memset(l, 0, sizeof(*l));
        c->ret = 0;
        for (int i = 0; i < LOCAL_MAX; i++) {
            if (s_local[i].state != L_FREE) {
                c->ret = 1; /* others are still open */
            }
        }
        if (c->ret == 0 && s_wake_tx) {
            udp_remove(s_wake_tx);
            s_wake_tx = NULL;
        }
        break;
    }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static int local_call(local_call_t *c)
{
    return tcpip_callback_wait(local_do, c) == ERR_OK ? c->ret : -1;
}
#else
/* ESP-IDF 4.4's lwIP has no tcpip_callback_wait; tcpip_api_call waits the same way. */
static err_t local_api(struct tcpip_api_call_data *call)
{
    local_do(call);
    return ERR_OK;
}

static int local_call(local_call_t *c)
{
    return tcpip_api_call(local_api, &c->call) == ERR_OK ? c->ret : -1;
}
#endif

static void local_wake_close(void)
{
    if (s_wake_fd >= 0) {
        close(s_wake_fd);
        s_wake_fd = -1;
    }
}

int ps_local_open(uint16_t port)
{
    if (s_wake_fd < 0) {
        struct sockaddr_in addr = { .sin_family = AF_INET };
        socklen_t len = sizeof(addr);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
            if (fd >= 0) {
                close(fd);
            }
            return -1;
        }
        ps_set_nonblock(fd);
        s_wake_port = ntohs(addr.sin_port);
        s_wake_fd = fd;
    }
    local_call_t c = { .op = LC_OPEN, .h = -1, .port = port };
    int h = local_call(&c);
    if (h < 0) {
        bool any = false;
        for (int i = 0; i < LOCAL_MAX; i++) {
            any = any || s_local[i].state != L_FREE;
        }
        if (!any) {
            local_wake_close();
        }
    }
    return h;
}

int ps_local_state(int h)
{
    uint8_t st = s_local[h].state;
    return st == L_OPEN ? 1 : st == L_CONNECTING ? 0 : -1;
}

void ps_local_want(int h, size_t bytes) { s_local[h].want = (uint16_t)bytes; }

/* Only a hint: lwIP's thread may be adding to the queue at this moment. */
int ps_local_avail(int h)
{
    struct pbuf *rx = s_local[h].rx;
    return rx ? rx->tot_len : 0;
}

int ps_local_read(int h, void *buf, size_t len)
{
    local_t *l = &s_local[h];
    if (!l->rx && !l->eof && l->pcb) {
        return 0; /* nothing queued: no need to ask lwIP's thread */
    }
    local_call_t c = { .op = LC_READ, .h = h, .buf = buf, .len = len };
    return local_call(&c);
}

int ps_local_write(int h, const void *buf, size_t len)
{
    local_call_t c = { .op = LC_WRITE, .h = h, .buf = (void *)buf, .len = len };
    return local_call(&c);
}

void ps_local_close(int h)
{
    local_call_t c = { .op = LC_CLOSE, .h = h };
    if (local_call(&c) == 0) {
        local_wake_close(); /* the last one */
    }
}

void ps_local_watch(int h, bool connecting, bool want_read, bool want_write,
                    fd_set *r, fd_set *w, fd_set *e, int *maxfd)
{
    s_sleeping = true; /* from here on, news sends the datagram */
    if (s_wake_fd >= 0) {
        FD_SET(s_wake_fd, r);
        if (s_wake_fd > *maxfd) {
            *maxfd = s_wake_fd;
        }
    }
}

void ps_local_flush(void)
{
    if (s_unflushed) {
        local_call_t c = { .op = LC_FLUSH, .h = -1 };
        s_unflushed = false;
        local_call(&c);
    }
}

/* Set after ps_local_watch() raised s_sleeping, so nothing falls between
 * this check and select(): later news sends the datagram. */
bool ps_local_ready(void)
{
    for (int i = 0; i < LOCAL_MAX; i++) {
        if (s_local[i].state != L_FREE && s_local[i].news) {
            return true;
        }
    }
    return false;
}

void ps_local_poll(const fd_set *readable)
{
    char b[4];
    s_sleeping = false;
    /* Cleared before the task looks: news that arrives meanwhile sets it again. */
    for (int i = 0; i < LOCAL_MAX; i++) {
        s_local[i].news = false;
    }
    if (s_wake_fd >= 0 && (s_wake_sent || FD_ISSET(s_wake_fd, readable))) {
        s_wake_sent = false;
        recv(s_wake_fd, b, sizeof(b), 0);
    }
}

/* The interface that carries traffic: the default one (ESP-IDF 5), or the
 * WiFi station, then Ethernet (ESP-IDF 4.4 has no "default"). */
static esp_netif_t *default_netif(void)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    return esp_netif_get_default_netif();
#else
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return netif ? netif : esp_netif_get_handle_from_ifkey("ETH_DEF");
#endif
}

/* Trying to connect before the network has an address only fills the log
 * with errors; the got-IP event cuts the wait short once it does. */
bool ps_port_network_ready(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = default_netif();
    return netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

/* ---- name lookup ----
 * One A query to the DNS server the network gave us. lwIP's own resolver
 * (behind getaddrinfo) keeps a 1.2 KB cache in RAM for the life of the
 * program; the service is looked up once per connection, so this keeps none. */

#define DNS_PORT  53
#define DNS_TRIES 3
#define DNS_WAIT_MS 2000

static size_t dns_query(uint8_t *q, size_t cap, const char *host, uint16_t id)
{
    size_t n = 12;
    if (strlen(host) > 253 || cap < 12 + strlen(host) + 2 + 4) {
        return 0;
    }
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01; /* recursion desired */
    q[5] = 1;    /* one question */
    while (*host) {
        size_t l = strcspn(host, ".");
        if (l == 0 || l > 63) {
            return 0;
        }
        q[n++] = (uint8_t)l;
        memcpy(q + n, host, l);
        n += l;
        host += l + (host[l] == '.');
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1; /* type A */
    q[n++] = 0; q[n++] = 1; /* class IN */
    return n;
}

/* Offset just past the name at `at`, 0 when it runs off the message. */
static size_t dns_skip_name(const uint8_t *m, size_t len, size_t at)
{
    while (at < len) {
        uint8_t l = m[at];
        if ((l & 0xc0) == 0xc0) {
            return at + 2 <= len ? at + 2 : 0; /* compressed: a pointer ends it */
        }
        if (l & 0xc0) {
            return 0; /* reserved label type */
        }
        at += 1 + l;
        if (l == 0) {
            return at;
        }
    }
    return 0;
}

static int dns_answer(const uint8_t *m, size_t len, uint16_t id, uint32_t *addr)
{
    if (len < 12 || ((m[0] << 8) | m[1]) != id || !(m[2] & 0x80) || (m[3] & 0x0f) != 0) {
        return -1;
    }
    unsigned qd = (m[4] << 8) | m[5], an = (m[6] << 8) | m[7];
    size_t at = 12;
    while (qd--) {
        at = dns_skip_name(m, len, at);
        if (!at || (at += 4) > len) {
            return -1;
        }
    }
    while (an--) {
        at = dns_skip_name(m, len, at);
        if (!at || at + 10 > len) {
            return -1;
        }
        unsigned type = (m[at] << 8) | m[at + 1], cls = (m[at + 2] << 8) | m[at + 3];
        unsigned rdlen = (m[at + 8] << 8) | m[at + 9];
        at += 10;
        if (at + rdlen > len) {
            return -1;
        }
        if (type == 1 && cls == 1 && rdlen == 4) { /* A, IN; CNAMEs before it are skipped */
            memcpy(addr, m + at, 4);
            return 0;
        }
        at += rdlen;
    }
    return -1;
}

int ps_port_resolve(const char *host, uint8_t *scratch, size_t len, uint32_t *addr)
{
    struct in_addr literal;
    if (inet_aton(host, &literal)) {
        *addr = literal.s_addr;
        return 0;
    }
    esp_netif_dns_info_t dns;
    esp_netif_t *netif = default_netif();
    if (!netif || esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
        dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
        return -1;
    }
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(DNS_PORT) };
    to.sin_addr.s_addr = dns.ip.u_addr.ip4.addr;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    int rc = -1;
    for (int t = 0; t < DNS_TRIES && rc != 0; t++) {
        uint16_t id;
        esp_fill_random(&id, sizeof(id));
        size_t n = dns_query(scratch, len, host, id);
        if (!n || sendto(fd, scratch, n, 0, (struct sockaddr *)&to, sizeof(to)) != (int)n) {
            break;
        }
        uint32_t start = ps_port_millis();
        while (rc != 0 && (uint32_t)(ps_port_millis() - start) < DNS_WAIT_MS) {
            fd_set r;
            struct timeval tv = { 0, 200 * 1000 };
            FD_ZERO(&r);
            FD_SET(fd, &r);
            if (select(fd + 1, &r, NULL, NULL, &tv) > 0) {
                int got = recv(fd, scratch, len, 0);
                rc = got > 0 ? dns_answer(scratch, (size_t)got, id, addr) : -1;
            }
        }
    }
    close(fd);
    return rc;
}

#endif /* ESP_PLATFORM */
