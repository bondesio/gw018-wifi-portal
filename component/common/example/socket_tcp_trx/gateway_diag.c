/* Local prototype: selected application diagnostics, not a ROM log mirror. */
#include "FreeRTOS.h"
#include "task.h"
#include <platform/platform_stdlib.h>
#include <lwip/sockets.h>
#include <lwip_netconf.h>

extern struct netif xnetif[NET_IF_NUM];
#include "gateway_diag.h"

#define DIAG_PORT 81
#define DIAG_HISTORY 32U
#define DIAG_EARLY_HISTORY 8U
#define DIAG_LINE_SIZE 512U
#define DIAG_POLL_MS 100U
#define DIAG_STATS_MS 5000U
#define DIAG_STALL_MS 5000U
#define DIAG_HEARTBEAT_MS 1000U
#define DIAG_STACK_MS 5000U
#define DIAG_TELEMETRY_LINES (2U + GW_DIAG_ROLE_COUNT)

struct diag_health {
    unsigned long alive;
    enum gateway_diag_task_state state;
    unsigned long state_since;
    unsigned long heartbeat;
    unsigned long last_progress;
    unsigned long stack_free_bytes;
    unsigned long generation;
};

/* Only the owning task writes its slot; snapshots are fixed-size copies.
 * These records survive task deletion, but never hold a TaskHandle. */
static struct diag_health health[GW_DIAG_ROLE_COUNT];
static TickType_t last_stack_sample[GW_DIAG_ROLE_COUNT];

static const char *const role_names[GW_DIAG_ROLE_COUNT] = {
    "tx", "rx", "bridge", "diag"
};
static const char *const state_names[GW_DIAG_STATE_COUNT] = {
    "stopped", "idle", "uart_sem", "uart_read", "uart_write", "tcp_sem",
    "tcp_send", "tcp_recv", "accept", "wait_tasks", "network_wait", "delay", "tcpip"
};

struct diag_record {
    unsigned long seq;
    unsigned long tick;
    unsigned long value;
    enum gateway_diag_event event;
};

static struct diag_record history[DIAG_HISTORY];
static struct diag_record early_history[DIAG_EARLY_HISTORY];
static unsigned long next_seq;
static unsigned int history_count, early_count;
static int history_has_wrapped;
static unsigned long counters[GW_DIAG_COUNTER_COUNT];
static unsigned long log_connections, log_disconnects, log_errors, log_lost;
static int started;
static volatile int network_ready;

static const char *const event_names[GW_DIAG_EVENT_COUNT] = {
    "app_start", "network_start", "uart_ready", "bridge_start",
    "bridge_listen", "bridge_connect", "bridge_disconnect", "bridge_error",
    "tx_exit", "rx_exit", "task_error", "network_state", "log_listen", "log_error", "network_ready"
};

static const char *const counter_names[GW_DIAG_COUNTER_COUNT] = {
    "uart_rx", "tcp_tx", "tcp_rx", "uart_tx", "send_errors", "recv_errors",
    "short_sends", "uart_buffer_drops", "bridge_connections", "bridge_disconnects"
};

struct diag_line {
    char *data;
    size_t capacity;
    unsigned int length;
    int truncated;
};

/* The SDK's _rtl_snprintf supports neither %u nor %lu. Avoid its restricted
 * varargs formatter and never produce UART warnings while formatting logs. */
static void line_init(struct diag_line *line, char *data, size_t capacity)
{
    line->data = data;
    line->capacity = capacity;
    line->length = 0;
    line->truncated = capacity == 0;
    if (capacity)
        data[0] = '\0';
}

static void line_char(struct diag_line *line, char character)
{
    if (line->length + 1U < line->capacity) {
        line->data[line->length++] = character;
        line->data[line->length] = '\0';
    } else {
        line->truncated = 1;
    }
}

static void line_text(struct diag_line *line, const char *text)
{
    while (*text)
        line_char(line, *text++);
}

static void line_number(struct diag_line *line, unsigned long value)
{
    char digits[3 * sizeof(unsigned long)];
    unsigned int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10UL);
        value /= 10UL;
    } while (value);
    while (count)
        line_char(line, digits[--count]);
}

static void line_field(struct diag_line *line, const char *name, unsigned long value)
{
    line_char(line, ' ');
    line_text(line, name);
    line_char(line, '=');
    line_number(line, value);
}

static int line_finish(struct diag_line *line)
{
    line_char(line, '\n');
    return line->truncated ? 0 : (int)line->length;
}

static void append_event(enum gateway_diag_event event, unsigned long value,
                         unsigned long tick)
{
    struct diag_record *record;
    record = &history[next_seq % DIAG_HISTORY];
    record->seq = next_seq++;
    record->tick = tick;
    record->event = event;
    record->value = value;
    if (early_count < DIAG_EARLY_HISTORY)
        early_history[early_count++] = *record;
    if (next_seq == 0)
        history_has_wrapped = 1;
    if (history_count < DIAG_HISTORY)
        history_count++;
}

/* Before scheduling, FreeRTOS critical nesting has a sentinel value and its
 * exit routine can leave IRQs disabled. Startup has one writer: append with
 * timestamp zero and no RTOS calls or changes to the interrupt mask. */
void gateway_diag_boot_event(enum gateway_diag_event event, unsigned long value)
{
    if ((unsigned int)event >= GW_DIAG_EVENT_COUNT)
        return;
    append_event(event, value, 0);
}

/* Only fixed-size copies and integer operations under the critical section.
 * Formatting, network I/O, allocation and waits never run in a producer. */
void gateway_diag_event(enum gateway_diag_event event, unsigned long value)
{
    if ((unsigned int)event >= GW_DIAG_EVENT_COUNT)
        return;
    taskENTER_CRITICAL();
    append_event(event, value, (unsigned long)xTaskGetTickCount());
    taskEXIT_CRITICAL();
}

/* Runs in the initialized TCPIP task. Publish only after recording completion;
 * no Wi-Fi driver query is needed, including during portal off/on transitions. */
void gateway_diag_network_ready(void *unused)
{
    (void)unused;
    gateway_diag_event(GW_DIAG_NETWORK_READY, 1);
    network_ready = 1;
}

/* Raw initialized lwIP interface state, never an association-status claim.
 * Read at most the station and optional AP interface; never expose addresses. */
static unsigned long network_state_snapshot(void)
{
    unsigned long state = 0;
    unsigned int i;
    taskENTER_CRITICAL();
    for (i = 0; i < NET_IF_NUM && i < 2U; ++i) {
        unsigned long flags = (netif_is_up(&xnetif[i]) ? 1UL : 0UL) |
            (netif_is_link_up(&xnetif[i]) ? 2UL : 0UL) |
            (ip_addr_get_ip4_u32(netif_ip_addr4(&xnetif[i])) ? 4UL : 0UL);
        state |= flags << (i * 3U);
    }
    taskEXIT_CRITICAL();
    return state;
}

static unsigned long saturated_add(unsigned long current, unsigned long value)
{
    return value > (~0UL - current) ? ~0UL : current + value;
}

void gateway_diag_add(enum gateway_diag_counter counter, unsigned long value)
{
    if ((unsigned int)counter >= GW_DIAG_COUNTER_COUNT)
        return;
    taskENTER_CRITICAL();
    counters[counter] = saturated_add(counters[counter], value);
    taskEXIT_CRITICAL();
}

void gateway_diag_task_start(enum gateway_diag_role role)
{
    TickType_t now;
    unsigned long stack_free;
    if ((unsigned int)role >= GW_DIAG_ROLE_COUNT)
        return;
    now = xTaskGetTickCount();
    /* The stack walker is deliberately outside the interrupt-masked copy. */
    stack_free = (unsigned long)uxTaskGetStackHighWaterMark(NULL) *
        (unsigned long)sizeof(StackType_t);
    taskENTER_CRITICAL();
    health[role].alive = 1;
    health[role].state = GW_DIAG_STATE_IDLE;
    health[role].state_since = (unsigned long)now;
    health[role].heartbeat = (unsigned long)now;
    health[role].last_progress = 0;
    health[role].stack_free_bytes = stack_free;
    health[role].generation++;
    last_stack_sample[role] = now;
    taskEXIT_CRITICAL();
}

void gateway_diag_task_state(enum gateway_diag_role role,
                             enum gateway_diag_task_state state)
{
    TickType_t now;
    if ((unsigned int)role >= GW_DIAG_ROLE_COUNT ||
        (unsigned int)state >= GW_DIAG_STATE_COUNT)
        return;
    /* One writer per role: unchanged markers need no clock read or lock. */
    if (health[role].state == state)
        return;
    now = xTaskGetTickCount();
    taskENTER_CRITICAL();
    health[role].state = state;
    health[role].state_since = (unsigned long)now;
    taskEXIT_CRITICAL();
}

void gateway_diag_task_heartbeat(enum gateway_diag_role role)
{
    TickType_t now;
    if ((unsigned int)role >= GW_DIAG_ROLE_COUNT)
        return;
    now = xTaskGetTickCount();
    if ((TickType_t)(now - (TickType_t)health[role].heartbeat) >=
        pdMS_TO_TICKS(DIAG_HEARTBEAT_MS)) {
        taskENTER_CRITICAL();
        health[role].heartbeat = (unsigned long)now;
        taskEXIT_CRITICAL();
    }
    if ((TickType_t)(now - last_stack_sample[role]) >=
        pdMS_TO_TICKS(DIAG_STACK_MS)) {
        unsigned long stack_free =
            (unsigned long)uxTaskGetStackHighWaterMark(NULL) *
            (unsigned long)sizeof(StackType_t);
        taskENTER_CRITICAL();
        health[role].stack_free_bytes = stack_free;
        last_stack_sample[role] = now;
        taskEXIT_CRITICAL();
    }
}

void gateway_diag_task_progress(enum gateway_diag_role role)
{
    TickType_t now;
    if ((unsigned int)role >= GW_DIAG_ROLE_COUNT)
        return;
    now = xTaskGetTickCount();
    /* Multiple UART bytes often complete in one tick. Keep the timestamp
     * exact without taking another copy lock for the identical value. */
    if (health[role].last_progress == (unsigned long)now)
        return;
    taskENTER_CRITICAL();
    health[role].last_progress = (unsigned long)now;
    taskEXIT_CRITICAL();
}

void gateway_diag_task_stop(enum gateway_diag_role role)
{
    TickType_t now;
    if ((unsigned int)role >= GW_DIAG_ROLE_COUNT)
        return;
    /* Retain the last allowed self-measurement, never inspect a dead task. */
    gateway_diag_task_heartbeat(role);
    now = xTaskGetTickCount();
    taskENTER_CRITICAL();
    health[role].alive = 0;
    health[role].state = GW_DIAG_STATE_STOPPED;
    health[role].state_since = (unsigned long)now;
    health[role].heartbeat = (unsigned long)now;
    taskEXIT_CRITICAL();
}

/* Copy a record before releasing the lock; senders never reference live slots.
 * Unsigned subtraction also handles sequence-number rollover. */
static int read_record(unsigned long *cursor, struct diag_record *record,
                       unsigned long *lost)
{
    unsigned long available;
    int result = 0;
    *lost = 0;
    taskENTER_CRITICAL();
    available = next_seq - *cursor;
    if (available > history_count) {
        *lost = available - history_count;
        log_lost = saturated_add(log_lost, *lost);
        *cursor = next_seq - history_count;
    }
    if (*cursor != next_seq) {
        *record = history[*cursor % DIAG_HISTORY];
        (*cursor)++;
        result = 1;
    }
    taskEXIT_CRITICAL();
    return result;
}

static int stats_line(char *line, size_t size)
{
    unsigned long c[GW_DIAG_COUNTER_COUNT];
    unsigned int i;
    struct diag_line output;
    taskENTER_CRITICAL();
    memcpy(c, counters, sizeof(c));
    taskEXIT_CRITICAL();
    line_init(&output, line, size);
    line_text(&output, "stats");
    line_field(&output, "tick", (unsigned long)xTaskGetTickCount());
    for (i = 0; i < GW_DIAG_COUNTER_COUNT; i++)
        line_field(&output, counter_names[i], c[i]);
    line_field(&output, "log_connections", log_connections);
    line_field(&output, "log_disconnects", log_disconnects);
    line_field(&output, "log_errors", log_errors);
    line_field(&output, "log_lost", log_lost);
    return line_finish(&output);
}

static int memory_line(char *line, size_t size)
{
    struct diag_line output;
    /* Both HP makefiles select heap_5.c. Its free/min APIs cover all regions.
     * No allocation, stack walk or formatting occurs in our critical section. */
    unsigned long free_heap, min_heap;
    taskENTER_CRITICAL();
    free_heap = (unsigned long)xPortGetFreeHeapSize();
    min_heap = (unsigned long)xPortGetMinimumEverFreeHeapSize();
    taskEXIT_CRITICAL();
    line_init(&output, line, size);
    line_text(&output, "memory");
    line_field(&output, "tick", (unsigned long)xTaskGetTickCount());
    line_field(&output, "free_heap", free_heap);
    line_field(&output, "min_heap", min_heap);
    return line_finish(&output);
}

static int health_line(char *line, size_t size, enum gateway_diag_role role)
{
    struct diag_health snapshot;
    struct diag_line output;
    taskENTER_CRITICAL();
    snapshot = health[role];
    taskEXIT_CRITICAL();
    line_init(&output, line, size);
    line_text(&output, "health");
    line_field(&output, "tick", (unsigned long)xTaskGetTickCount());
    line_text(&output, " role=");
    line_text(&output, role_names[role]);
    line_field(&output, "alive", snapshot.alive);
    line_text(&output, " state=");
    line_text(&output, state_names[snapshot.state]);
    line_field(&output, "state_since", snapshot.state_since);
    line_field(&output, "heartbeat", snapshot.heartbeat);
    line_field(&output, "last_progress", snapshot.last_progress);
    line_field(&output, "stack_free_bytes", snapshot.stack_free_bytes);
    line_field(&output, "generation", snapshot.generation);
    return line_finish(&output);
}

static int telemetry_line(char *line, size_t size, unsigned int index)
{
    if (index == 0)
        return stats_line(line, size);
    if (index == 1)
        return memory_line(line, size);
    return health_line(line, size, (enum gateway_diag_role)(index - 2U));
}

static int event_line(char *line, size_t size, const struct diag_record *record,
                      unsigned long lost)
{
    struct diag_line output;
    line_init(&output, line, size);
    line_text(&output, "event");
    line_field(&output, "seq", record->seq);
    line_field(&output, "tick", record->tick);
    line_text(&output, " name=");
    line_text(&output, event_names[record->event]);
    line_field(&output, "value", record->value);
    line_field(&output, "lost", lost);
    return line_finish(&output);
}

static int header_line(char *line, size_t size)
{
    struct diag_line output;
    line_init(&output, line, size);
    line_text(&output, "gateway_diag version=3 port=81");
    line_field(&output, "tick_hz", (unsigned long)configTICK_RATE_HZ);
    line_text(&output, " history=32 early=8 scope=application netif_bits=sta_up:1,sta_link:2,sta_ip:4,ap_up:8,ap_link:16,ap_ip:32");
    return line_finish(&output);
}

static int socket_can_retry(int fd)
{
    int error = 0;
    socklen_t size = sizeof(error);
    gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_TCPIP);
    /* This SDK disables newlib task reentrancy: global errno can be changed by
     * the bridge task. Its lwIP also does not set SO_ERROR for EWOULDBLOCK.
     * Query this socket's pending error, accepting zero as a retry condition. */
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
        (error == 0 || error == EAGAIN || error == EWOULDBLOCK);
}

static int make_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int nonblock = 1;
    struct sockaddr_in address;
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(DIAG_PORT);
    address.sin_addr.s_addr = INADDR_ANY;
    if (ioctl(fd, FIONBIO, &nonblock) != 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    gateway_diag_event(GW_DIAG_LOG_LISTEN, DIAG_PORT);
    return fd;
}

static void diag_thread(void *param)
{
    int server = -1, client = -1, client_closing = 0, rejected = -1;
    unsigned long previous_network_state = ~0UL;
    char line[DIAG_LINE_SIZE], discard[32];
    unsigned int length = 0, offset = 0;
    unsigned int early_index = 0, early_limit = 0;
    int last_line_telemetry = 0;
    unsigned int telemetry_index = DIAG_TELEMETRY_LINES;
    unsigned long cursor = 0;
    TickType_t last_stats = 0, last_progress = 0, last_listen = 0;
    (void)param;

    gateway_diag_task_start(GW_DIAG_ROLE_DIAG);
    gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_NETWORK_WAIT);

    /* No socket or netif operation may precede the TCPIP readiness callback. */
    while (!network_ready) {
        gateway_diag_task_heartbeat(GW_DIAG_ROLE_DIAG);
        vTaskDelay(pdMS_TO_TICKS(DIAG_POLL_MS));
    }

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        gateway_diag_task_heartbeat(GW_DIAG_ROLE_DIAG);
        gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_IDLE);
        unsigned long state = network_state_snapshot();
        if (state != previous_network_state) {
            previous_network_state = state;
            gateway_diag_event(GW_DIAG_NETWORK_STATE, state);
        }
        /* Retry after failure without flooding the ring or consuming CPU. */
        if (server < 0 && (last_listen == 0 ||
            (TickType_t)(now - last_listen) >= pdMS_TO_TICKS(DIAG_STATS_MS))) {
            last_listen = now ? now : 1;
            gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_TCPIP);
            server = make_listener();
            if (server < 0) {
                log_errors = saturated_add(log_errors, 1);
                gateway_diag_event(GW_DIAG_LOG_ERROR, 1);
            }
        }
        /* Never lose ownership if the TCPIP mailbox temporarily rejects an
         * abort request. Retry the one pending rejected peer before accepting. */
        if (rejected >= 0) {
            gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_TCPIP);
            if (lwip_abortclose(rejected) == 0)
                rejected = -1;
        }
        if (server >= 0 && client < 0 && rejected < 0) {
            gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_ACCEPT);
            client = accept(server, NULL, NULL);
            if (client >= 0) {
                int nonblock = 1;
                gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_TCPIP);
                if (ioctl(client, FIONBIO, &nonblock) != 0) {
                    client_closing = 1;
                    log_errors = saturated_add(log_errors, 1);
                } else {
                    log_connections = saturated_add(log_connections, 1);
                    taskENTER_CRITICAL();
                    early_limit = early_count;
                    /* Starting after the pinned records avoids duplicate
                     * replay; read_record reports any gap to the live ring.
                     * After sequence rollover, use the current ring epoch. */
                    cursor = history_has_wrapped ? next_seq - history_count :
                        (unsigned long)early_limit;
                    taskEXIT_CRITICAL();
                    early_index = 0;
                    last_line_telemetry = 0;
                    telemetry_index = DIAG_TELEMETRY_LINES;
                    length = (unsigned int)header_line(line, sizeof(line));
                    offset = 0;
                    last_stats = now - pdMS_TO_TICKS(DIAG_STATS_MS);
                    last_progress = now;
                }
            } else if (!socket_can_retry(server)) {
                close(server);
                server = -1;
                log_errors = saturated_add(log_errors, 1);
                gateway_diag_event(GW_DIAG_LOG_ERROR, 2);
            }
        }
        /* Drain one queued extra peer per iteration even while serving a slow
         * reader. Bounded work prevents backlog clients monopolizing the task. */
        if (server >= 0 && client >= 0 && !client_closing && rejected < 0) {
            gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_ACCEPT);
            int extra = accept(server, NULL, NULL);
            if (extra >= 0) {
                gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_TCPIP);
                if (lwip_abortclose(extra) != 0)
                    rejected = extra;
            }
        }
        if (client_closing)
            goto disconnect;
        if (client >= 0) {
            gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_TCP_RECV);
            int ret = recv(client, discard, sizeof(discard), MSG_DONTWAIT);
            if (ret > 0)
                gateway_diag_task_progress(GW_DIAG_ROLE_DIAG);
            /* Input is discarded. This endpoint has no control protocol. */
            if (ret == 0 || (ret < 0 && !socket_can_retry(client))) {
                if (ret < 0)
                    log_errors = saturated_add(log_errors, 1);
                goto disconnect;
            }
            if (offset == length) {
                int size;
                struct diag_record record;
                unsigned long lost;
                int stats_due = (TickType_t)(now - last_stats) >=
                    pdMS_TO_TICKS(DIAG_STATS_MS);
                if (telemetry_index == DIAG_TELEMETRY_LINES && stats_due) {
                    telemetry_index = 0;
                    last_stats = now;
                }
                if (early_index < early_limit) {
                    taskENTER_CRITICAL();
                    record = early_history[early_index++];
                    taskEXIT_CRITICAL();
                    size = event_line(line, sizeof(line), &record, 0);
                    last_line_telemetry = 0;
                } else if (telemetry_index < DIAG_TELEMETRY_LINES &&
                           !last_line_telemetry) {
                    size = telemetry_line(line, sizeof(line), telemetry_index++);
                    last_line_telemetry = 1;
                } else if (read_record(&cursor, &record, &lost)) {
                    size = event_line(line, sizeof(line), &record, lost);
                    last_line_telemetry = 0;
                } else if (telemetry_index < DIAG_TELEMETRY_LINES) {
                    size = telemetry_line(line, sizeof(line), telemetry_index++);
                    last_line_telemetry = 1;
                } else {
                    size = 0;
                }
                /* The bounded formatter discards a truncated line. */
                length = size > 0 && (unsigned int)size < sizeof(line) ?
                    (unsigned int)size : 0;
                offset = 0;
            }
            if (offset < length) {
                gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_TCP_SEND);
                int can_send = lwip_diag_can_send(client);
                if (can_send < 0) {
                    log_errors = saturated_add(log_errors, 1);
                    goto disconnect;
                }
                /* At most one bounded chunk may be unacknowledged. Local
                 * send success alone cannot keep an absent reader alive. */
                ret = can_send ? send(client, line + offset, length - offset,
                    MSG_DONTWAIT) : -1;
                if (ret > 0) {
                    offset += (unsigned int)ret;
                    last_progress = now;
                    gateway_diag_task_progress(GW_DIAG_ROLE_DIAG);
                } else if (can_send && (ret == 0 || !socket_can_retry(client))) {
                    log_errors = saturated_add(log_errors, 1);
                    goto disconnect;
                }
                if ((TickType_t)(now - last_progress) >= pdMS_TO_TICKS(DIAG_STALL_MS))
                    goto disconnect;
            }
        }
        gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_DELAY);
        vTaskDelay(pdMS_TO_TICKS(DIAG_POLL_MS));
        continue;

disconnect:
        client_closing = 1;
        gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_TCPIP);
        if (lwip_abortclose(client) != 0) {
            gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_DELAY);
            vTaskDelay(pdMS_TO_TICKS(DIAG_POLL_MS));
            continue;
        }
        client = -1;
        client_closing = 0;
        log_disconnects = saturated_add(log_disconnects, 1);
        length = offset = 0;
        gateway_diag_task_state(GW_DIAG_ROLE_DIAG, GW_DIAG_STATE_DELAY);
        vTaskDelay(pdMS_TO_TICKS(DIAG_POLL_MS));
    }
}

void gateway_diag_start(void)
{
    if (started)
        return;
    /* Equal to bridge task priority; SDK enables time slicing. The task yields
     * every iteration and performs at most one nonblocking send per iteration. */
    if (xTaskCreate(diag_thread, "gateway_diag", 1024, NULL,
                    tskIDLE_PRIORITY + 1, NULL) == pdPASS)
        started = 1;
    else
        gateway_diag_boot_event(GW_DIAG_TASK_ERROR, 81);
}
