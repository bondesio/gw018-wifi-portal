/* Deterministic host simulation. Includes the production module so ring and
 * connection-state tests exercise the actual implementation. No gateway I/O. */
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include "gateway_diag.c"

struct netif xnetif[NET_IF_NUM];
static int netif_reads, extra_closed, abort_attempts;
static TickType_t tick, closed_tick;
static unsigned int stack_calls, stack_words = 128;
static size_t free_heap = 100000, min_heap = 80000;
static int critical, socket_calls, iterations, limit, accepted, client_closed;
static unsigned long critical_nesting;
static int primask, critical_entries, tick_reads;
static int task_creates, task_result, scenario, send_calls;
static unsigned int server_port;
static char output[32768];
static size_t output_size;
static jmp_buf done;

/* Model RTL8721D HP's pre-scheduler sentinel nesting and PRIMASK semantics. */
void test_enter(void)
{
    primask = 1;
    critical_nesting++;
    critical++;
    critical_entries++;
}
void test_exit(void)
{
    assert(critical > 0 && critical_nesting > 0);
    critical--;
    if (!--critical_nesting)
        primask = 0;
}
TickType_t xTaskGetTickCount(void) { tick_reads++; return tick; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    assert(task == NULL && !critical);
    stack_calls++;
    return stack_words;
}
size_t xPortGetFreeHeapSize(void) { assert(network_ready); return free_heap; }
size_t xPortGetMinimumEverFreeHeapSize(void) { assert(network_ready); return min_heap; }

void vTaskDelay(TickType_t ticks)
{
    assert(critical == 0);
    if (scenario == 10 && iterations == 2)
        gateway_diag_network_ready(NULL);
    if (iterations == 2) {
        xnetif[0].flags = 3;
        xnetif[0].ip_addr = 1;
    }
    if (scenario == 6)
        gateway_diag_event(GW_DIAG_BRIDGE_CONNECT, (unsigned long)iterations);
    tick += ticks;
    if (++iterations >= limit)
        longjmp(done, 1);
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned int stack,
                void *param, unsigned int priority, void *handle)
{
    assert(fn == diag_thread && !strcmp(name, "gateway_diag"));
    assert(stack == 1024 && param == NULL && priority == 1 && handle == NULL);
    task_creates++;
    return task_result;
}
unsigned int test_netif_flags(const struct netif *netif)
{ assert(network_ready); netif_reads++; return netif->flags; }
uint32_t test_netif_ip(const uint32_t *ip)
{ assert(network_ready); netif_reads++; return *ip; }
int test_socket(int domain, int type, int protocol)
{
    assert(!critical && domain == AF_INET && type == SOCK_STREAM && !protocol);
    assert(network_ready);
    socket_calls++;
    return scenario == 8 ? -1 : 100;
}
int test_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    assert(!critical && (fd == 100 || fd == 200) && request == FIONBIO);
    assert(*va_arg(args, int *) == 1);
    va_end(args);
    return 0;
}
int test_bind(int fd, const struct sockaddr *address, socklen_t size)
{
    const struct sockaddr_in *a = (const struct sockaddr_in *)address;
    assert(!critical && fd == 100 && size == sizeof(*a));
    server_port = ntohs(a->sin_port);
    return 0;
}
int test_listen(int fd, int backlog)
{ assert(!critical && fd == 100 && backlog == 1); return 0; }
int test_accept(int fd, struct sockaddr *address, socklen_t *size)
{
    assert(!critical && fd == 100 && address == NULL && size == NULL);
    if (!accepted++ || (scenario == 7 && client_closed && log_connections == 1))
        return 200;
    if ((scenario == 11 || scenario == 14) && accepted < 6)
        return 201;
    errno = EAGAIN;
    return -1;
}
int test_close(int fd)
{
    assert(!critical && (fd == 100 || fd == 200 || fd == 201));
    if (fd == 201)
        extra_closed++;
    if (fd == 200) {
        client_closed++;
        closed_tick = tick;
    }
    return 0;
}
int lwip_abortclose(int fd)
{
    abort_attempts++;
    if ((scenario == 13 || scenario == 14) && abort_attempts < 4)
        return -1;
    return test_close(fd);
}
int lwip_diag_can_send(int fd)
{
    assert(fd == 200 && !critical);
    return ((scenario == 12 || scenario == 15) && send_calls) ? 0 : 1;
}
int test_getsockopt(int fd, int level, int option, void *value, socklen_t *size)
{
    assert(!critical && (fd == 100 || fd == 200));
    assert(level == SOL_SOCKET && option == SO_ERROR && *size == sizeof(int));
    *(int *)value = fd == 200 && scenario == 3 && send_calls ? EPIPE :
        (fd == 200 && scenario == 9 ? ECONNRESET : 0);
    /* Simulate an unrelated task changing global errno. Retry classification
     * must use the socket's pending error, not this process-global value. */
    errno = EBADF;
    return 0;
}
ssize_t test_send(int fd, const void *data, size_t size, int flags)
{
    size_t chunk;
    assert(!critical && fd == 200 && flags == MSG_DONTWAIT);
    send_calls++;
    if (scenario == 1 || (scenario == 2 && send_calls % 3 == 0)) {
        errno = EAGAIN;
        return -1;
    }
    if (scenario == 3) {
        errno = EPIPE;
        return -1;
    }
    chunk = scenario == 15 ? size : (size > 7 ? 7 : size);
    assert(output_size + chunk < sizeof(output));
    memcpy(output + output_size, data, chunk);
    output_size += chunk;
    output[output_size] = 0;
    return (ssize_t)chunk;
}
ssize_t test_recv(int fd, void *data, size_t size, int flags)
{
    assert(!critical && fd == 200 && size == 32 && flags == MSG_DONTWAIT);
    if ((scenario == 4 || scenario == 13 || (scenario == 7 && !client_closed)) && iterations >= 2)
        return 0;
    if (scenario == 9) {
        errno = ECONNRESET;
        return -1;
    }
    if (scenario == 5 && iterations == 0) {
        memset(data, 'x', size);
        return (ssize_t)size;
    }
    errno = EAGAIN;
    return -1;
}

static void reset(void)
{
    memset(history, 0, sizeof(history));
    memset(health, 0, sizeof(health));
    memset(last_stack_sample, 0, sizeof(last_stack_sample));
    stack_calls = 0; stack_words = 128;
    free_heap = 100000; min_heap = 80000;
    memset(counters, 0, sizeof(counters));
    memset(early_history, 0, sizeof(early_history));
    next_seq = history_count = early_count = 0;
    history_has_wrapped = 0;
    log_connections = log_disconnects = log_errors = log_lost = 0;
    started = network_ready = netif_reads = extra_closed = abort_attempts = 0;
    memset(xnetif, 0, sizeof(xnetif));
    closed_tick = 0;
    tick = critical = socket_calls = iterations = accepted = client_closed = 0;
    critical_nesting = 0;
    primask = critical_entries = tick_reads = 0;
    task_creates = scenario = send_calls = 0;
    task_result = pdPASS;
    output_size = server_port = 0;
    output[0] = 0;
}

static void test_producers_and_ring(void)
{
    unsigned int i;
    unsigned long cursor = 0, lost;
    struct diag_record record;
    char line[DIAG_LINE_SIZE];
    int length;
    reset();
    gateway_diag_event(GW_DIAG_EVENT_COUNT, 0);
    gateway_diag_add(GW_DIAG_COUNTER_COUNT, 1);
    assert(next_seq == 0);
    for (i = 0; i < 40; ++i)
        gateway_diag_event(GW_DIAG_APP_START, i);
    assert(history_count == 32 && socket_calls == 0);
    assert(early_count == 8 && early_history[0].seq == 0 && early_history[7].seq == 7);
    assert(read_record(&cursor, &record, &lost) && lost == 8);
    assert(record.seq == 8 && record.value == 8 && cursor == 9);
    for (i = 9; i < 40; ++i) {
        assert(read_record(&cursor, &record, &lost) && !lost);
        assert(record.seq == i && record.value == i);
    }
    assert(!read_record(&cursor, &record, &lost));
    gateway_diag_add(GW_DIAG_UART_RX_BYTES, ULONG_MAX);
    gateway_diag_add(GW_DIAG_UART_RX_BYTES, 1);
    assert(counters[GW_DIAG_UART_RX_BYTES] == ULONG_MAX);
    for (i = 0; i < GW_DIAG_COUNTER_COUNT; ++i)
        gateway_diag_add((enum gateway_diag_counter)i, ULONG_MAX);
    log_connections = log_disconnects = log_errors = log_lost = ULONG_MAX;
    length = stats_line(line, sizeof(line));
    assert(length > 0 && (size_t)length < sizeof(line));
    assert(line[length - 1] == '\n' && socket_calls == 0 && !critical);
    assert(stats_line(line, 2) == 0 && line[1] == '\0');
    assert(stats_line(NULL, 0) == 0);
    length = header_line(line, sizeof(line));
    assert(length > 0 && (size_t)length < sizeof(line));
    assert(strstr(line, "netif_bits=sta_up:1,sta_link:2,sta_ip:4"));
    assert(header_line(line, 2) == 0);
    assert(header_line(NULL, 0) == 0);

    reset();
    next_seq = ULONG_MAX - 1;
    cursor = next_seq;
    for (i = 0; i < 4; ++i)
        gateway_diag_event(GW_DIAG_UART_READY, i);
    for (i = 0; i < 4; ++i) {
        assert(read_record(&cursor, &record, &lost) && !lost);
        assert(record.value == i);
    }
    assert(!read_record(&cursor, &record, &lost));
}

static void test_boot_interrupt_state(void)
{
    reset();
    critical_nesting = 0xaaaaaaaaUL;
    primask = 0;
    tick = 12345;
    gateway_diag_boot_event(GW_DIAG_APP_START, 0);
    gateway_diag_boot_event(GW_DIAG_NETWORK_START, 0);
    gateway_diag_boot_event(GW_DIAG_UART_READY, 115200);
    gateway_diag_boot_event(GW_DIAG_EVENT_COUNT, 0);
    assert(history_count == 3 && early_count == 3);
    assert(history[0].tick == 0 && history[2].tick == 0);
    assert(critical_nesting == 0xaaaaaaaaUL && primask == 0);
    assert(critical_entries == 0 && tick_reads == 0 && socket_calls == 0);
    /* Scheduler startup resets the sentinel; runtime producers use protection. */
    critical_nesting = 0;
    gateway_diag_event(GW_DIAG_BRIDGE_START, 80);
    gateway_diag_add(GW_DIAG_UART_RX_BYTES, 7);
    assert(critical_entries == 2 && !critical_nesting && !primask && !critical);
    assert(history[3].tick == 12345 && counters[GW_DIAG_UART_RX_BYTES] == 7);
}

static void run_thread(int mode, int loops)
{
    reset();
    scenario = mode;
    limit = loops;
    if (mode != 10)
        gateway_diag_network_ready(NULL);
    gateway_diag_event(GW_DIAG_APP_START, 0);
    gateway_diag_event(GW_DIAG_UART_READY, 115200);
    if (!setjmp(done))
        diag_thread(NULL);
    assert(!critical && server_port == 81 && log_connections >= 1);
}

static void test_pinned_replay(void)
{
    unsigned int i;
    const char *event;
    reset();
    limit = 500;
    for (i = 0; i < 50; ++i)
        gateway_diag_event(GW_DIAG_APP_START, i);
    gateway_diag_network_ready(NULL);
    if (!setjmp(done))
        diag_thread(NULL);
    /* First eight events survive churn. The later gap to the rolling ring is
     * explicit even though its records have already been overwritten. */
    event = output;
    for (i = 0; i < 8; ++i) {
        char needle[80];
        snprintf(needle, sizeof(needle), "event seq=%u tick=0 name=app_start value=%u lost=0\n", i, i);
        event = strstr(event, needle);
        assert(event);
        event += strlen(needle);
    }
    /* Readiness and the task add four state/listener events while replaying, so the retained
     * ring starts at seq 22 and reports a gap of 14. No pinned duplicates. */
    assert(strstr(event, "event seq=22 tick=0 name=app_start value=22 lost=14\n"));
    assert(!strstr(event, "event seq=0 "));
    assert(!strstr(output, "event seq=8 "));
    assert(log_lost == 14);
    event = output;
    {
        unsigned long previous = 0;
        int first = 1;
        while ((event = strstr(event, "event seq="))) {
            unsigned long sequence = strtoul(event + 10, NULL, 10);
            assert(first || sequence > previous);
            first = 0;
            previous = sequence;
            event++;
        }
    }
}

static void test_task_health(void)
{
    char line[DIAG_LINE_SIZE];
    int clocks;
    unsigned long events;
    int length;
    reset();
    tick = 100;
    gateway_diag_task_start(GW_DIAG_ROLE_TX);
    assert(health[GW_DIAG_ROLE_TX].alive == 1);
    assert(health[GW_DIAG_ROLE_TX].generation == 1);
    assert(health[GW_DIAG_ROLE_TX].stack_free_bytes == 128 * sizeof(StackType_t));
    assert(stack_calls == 1 && !critical);
    gateway_diag_task_state(GW_DIAG_ROLE_TX, GW_DIAG_STATE_UART_SEM);
    clocks = tick_reads;
    tick = 120;
    gateway_diag_task_state(GW_DIAG_ROLE_TX, GW_DIAG_STATE_UART_SEM);
    assert(tick_reads == clocks && health[GW_DIAG_ROLE_TX].state_since == 100);
    gateway_diag_task_state(GW_DIAG_ROLE_TX, GW_DIAG_STATE_UART_WRITE);
    gateway_diag_task_progress(GW_DIAG_ROLE_TX);
    assert(health[GW_DIAG_ROLE_TX].last_progress == 120);
    assert(health[GW_DIAG_ROLE_TX].state_since == 120);
    tick = 1099;
    gateway_diag_task_heartbeat(GW_DIAG_ROLE_TX);
    assert(health[GW_DIAG_ROLE_TX].heartbeat == 100 && stack_calls == 1);
    tick = 1100;
    gateway_diag_task_heartbeat(GW_DIAG_ROLE_TX);
    assert(health[GW_DIAG_ROLE_TX].heartbeat == 1100 && stack_calls == 1);
    stack_words = 100;
    tick = 5100;
    gateway_diag_task_heartbeat(GW_DIAG_ROLE_TX);
    assert(stack_calls == 2 && health[GW_DIAG_ROLE_TX].stack_free_bytes == 400);
    tick = 5101;
    gateway_diag_task_heartbeat(GW_DIAG_ROLE_TX);
    assert(stack_calls == 2);
    events = next_seq;
    clocks = tick_reads;
    gateway_diag_task_start(GW_DIAG_ROLE_COUNT);
    gateway_diag_task_state(GW_DIAG_ROLE_COUNT, GW_DIAG_STATE_IDLE);
    gateway_diag_task_state(GW_DIAG_ROLE_TX, GW_DIAG_STATE_COUNT);
    gateway_diag_task_heartbeat(GW_DIAG_ROLE_COUNT);
    gateway_diag_task_progress(GW_DIAG_ROLE_COUNT);
    gateway_diag_task_stop(GW_DIAG_ROLE_COUNT);
    assert(tick_reads == clocks && stack_calls == 2 && next_seq == events);
    tick = 30000; /* Blocked owner: observer sees frozen execution/progress. */
    length = health_line(line, sizeof(line), GW_DIAG_ROLE_TX);
    assert(length > 0 && strstr(line, "state=uart_write state_since=120"));
    assert(strstr(line, "heartbeat=5100 last_progress=120"));
    assert(stack_calls == 2 && !critical);
    gateway_diag_task_stop(GW_DIAG_ROLE_TX);
    assert(!health[GW_DIAG_ROLE_TX].alive && health[GW_DIAG_ROLE_TX].state == GW_DIAG_STATE_STOPPED);
    assert(health[GW_DIAG_ROLE_TX].last_progress == 120);
    assert(stack_calls == 3); /* stop may refresh a sample after the 5s interval */
    tick = 31000;
    gateway_diag_task_start(GW_DIAG_ROLE_TX);
    assert(health[GW_DIAG_ROLE_TX].generation == 2 && !health[GW_DIAG_ROLE_TX].last_progress);
    reset();
    tick = UINT32_MAX - 6000U;
    gateway_diag_task_start(GW_DIAG_ROLE_RX);
    tick = 500;
    gateway_diag_task_heartbeat(GW_DIAG_ROLE_RX);
    assert(stack_calls == 2 && health[GW_DIAG_ROLE_RX].heartbeat == 500);
}

static void test_health_formatters(void)
{
    unsigned int i;
    char line[DIAG_LINE_SIZE];
    reset();
    network_ready = 1;
    free_heap = min_heap = SIZE_MAX;
    assert(memory_line(line, sizeof(line)) > 0);
    assert(strstr(line, "memory tick="));
    assert(memory_line(line, 2) == 0 && line[1] == '\0');
    assert(memory_line(NULL, 0) == 0);
    for (i = 0; i < GW_DIAG_ROLE_COUNT; ++i) {
        health[i].alive = 1;
        health[i].state = GW_DIAG_STATE_TCP_SEND;
        health[i].state_since = health[i].heartbeat = health[i].last_progress = ULONG_MAX;
        health[i].stack_free_bytes = health[i].generation = ULONG_MAX;
        assert(health_line(line, sizeof(line), (enum gateway_diag_role)i) > 0);
        assert(strstr(line, "state=tcp_send") && line[strlen(line)-1] == '\n');
        assert(health_line(line, 2, (enum gateway_diag_role)i) == 0 && line[1] == '\0');
        assert(health_line(NULL, 0, (enum gateway_diag_role)i) == 0);
    }
    for (i = 0; i < DIAG_TELEMETRY_LINES; ++i)
        assert(telemetry_line(line, sizeof(line), i) > 0);
}

int main(void)
{
    test_task_health();
    test_health_formatters();
    test_producers_and_ring();
    test_boot_interrupt_state();
    test_pinned_replay();
    reset();
    gateway_diag_start();
    gateway_diag_start();
    assert(task_creates == 1 && started);
    reset();
    task_result = 0;
    gateway_diag_start();
    assert(!started && history_count == 1 && history[0].event == GW_DIAG_TASK_ERROR);

    run_thread(2, 450); /* partial writes plus intermittent backpressure */
    assert(!client_closed && !log_errors);
    assert(strstr(output, "gateway_diag version=3 port=81"));
    assert(strstr(output, "name=app_start value=0 lost=0\n"));
    assert(strstr(output, "name=uart_ready value=115200 lost=0\n"));
    assert(strstr(output, "name=network_state value=7 lost=0\n"));
    assert(strstr(output, "stats tick="));
    assert(strstr(output, "memory tick="));
    assert(strstr(output, "role=tx") && strstr(output, "role=rx"));
    assert(strstr(output, "role=bridge") && strstr(output, "role=diag"));
    run_thread(1, 60); /* no progress: bounded stalled-client timeout */
    assert(client_closed == 1 && log_disconnects == 1 && output_size == 0);
    run_thread(3, 3); /* send failure */
    assert(client_closed == 1 && log_errors == 1);
    run_thread(4, 4); /* graceful EOF */
    assert(client_closed == 1);
    run_thread(5, 3); /* input discarded */
    assert(!client_closed);
    assert(!strstr(output, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
    run_thread(6, 600); /* live-ring overrun with slow partial writes */
    assert(log_lost > 0 && strstr(output, "name=bridge_connect"));
    assert(strstr(output, "memory tick=") && strstr(output, "role=diag"));
    run_thread(7, 160); /* reconnect replays pinned startup events again */
    assert(client_closed == 1 && log_connections == 2);
    assert(strstr(output + 1, "gateway_diag version=3 port=81"));
    run_thread(9, 3); /* terminal receive error */
    assert(client_closed == 1 && log_errors == 1);
    reset();
    scenario = 8;
    limit = 120;
    gateway_diag_network_ready(NULL);
    if (!setjmp(done))
        diag_thread(NULL);
    assert(socket_calls == 3 && log_errors == 3 && log_connections == 0);
    reset();
    limit = 20;
    if (!setjmp(done))
        diag_thread(NULL);
    assert(!network_ready && !socket_calls && !netif_reads && !output_size);
    run_thread(10, 300); /* callback delayed: zero pre-initialization access */
    assert(network_ready && strstr(output, "name=network_ready value=1"));
    assert(strstr(output, "name=network_state value=7"));
    run_thread(11, 300); /* queued peers drained while current output progresses */
    assert(extra_closed == 4 && log_connections == 1 && !client_closed);
    assert(strstr(output, "name=network_state value=0"));
    assert(strstr(output, "name=network_state value=7"));
    assert(strstr(output, "stats tick="));
    run_thread(12, 80); /* ACK stall: one chunk then bounded timeout */
    assert(send_calls == 1 && output_size <= DIAG_LINE_SIZE);
    assert(client_closed == 1 && log_disconnects == 1);
    run_thread(15, 80); /* a complete but unacknowledged line still times out */
    assert(send_calls == 1 && client_closed == 1 && closed_tick == DIAG_STALL_MS);
    run_thread(13, 8); /* failed abort retains client ownership for retry */
    assert(abort_attempts == 4 && client_closed == 1 && log_disconnects == 1);
    run_thread(14, 300); /* failed extra-client abort also retains ownership */
    assert(extra_closed == 4 && !client_closed && log_connections == 1);
    reset();
    gateway_diag_network_ready(NULL);
    xnetif[0].flags = xnetif[1].flags = 3;
    xnetif[0].ip_addr = xnetif[1].ip_addr = 1;
    assert(network_state_snapshot() == 63);
    xnetif[0].flags = 0; xnetif[0].ip_addr = 0;
    assert(network_state_snapshot() == 56); /* AP only, station not ready */
    puts("gateway_diag host tests: PASS");
    return 0;
}
