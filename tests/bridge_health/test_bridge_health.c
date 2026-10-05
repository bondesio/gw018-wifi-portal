/* Include the actual source unchanged: this tests call-site ordering, not a
 * reimplementation of the bridge. No UART/network/RTOS operations escape. */
#include "host_platform.h"
#include "gateway_diag.h"
#include <setjmp.h>
#include "example_socket_tcp_trx_1.c"

enum operation { NONE, UART_SEM, TCP_SEM, GETC, PUTC, SEND, RECV,
                 ACCEPT, SELECT, WAIT_CHILDREN, START_DELAY, NETWORK_WAIT };
struct health {
    int alive, starts, stops, heartbeats, progress;
    unsigned int last_progress;
    enum gateway_diag_task_state state;
};
static struct health health[GW_DIAG_ROLE_COUNT];
static enum gateway_diag_role current_role;
static enum operation blocked;
static int suspend_after;
static jmp_buf escape;
static unsigned int clock_tick;
static int completed_io, uart_bytes, send_result, recv_result, socket_error;
static int socket_result, bind_result, listen_result, select_calls, accept_calls;
static int checks;
static unsigned long counters[GW_DIAG_COUNTER_COUNT];
struct netif xnetif[NET_IF_NUM];
static int wifi_ready;

static void marker(enum gateway_diag_task_state state) {
    assert(health[current_role].alive);
    assert(health[current_role].state == state);
    ++checks;
}
static void suspend(enum operation op) {
    if (blocked == op && --suspend_after == 0) {
        unsigned int stamp = health[current_role].last_progress;
        int count = health[current_role].progress;
        clock_tick += 30000; /* a wait must not invent fresh byte progress */
        assert(health[current_role].last_progress == stamp);
        assert(health[current_role].progress == count);
        longjmp(escape, 1);
    }
}
void gateway_diag_task_start(enum gateway_diag_role role) {
    assert(!health[role].alive);
    current_role = role;
    health[role].alive = 1;
    health[role].starts++;
    health[role].state = GW_DIAG_STATE_IDLE;
}
void gateway_diag_task_state(enum gateway_diag_role role, enum gateway_diag_task_state state) {
    assert(health[role].alive);
    health[role].state = state;
}
void gateway_diag_task_heartbeat(enum gateway_diag_role role) {
    assert(health[role].alive);
    health[role].heartbeats++;
    ++clock_tick;
}
void gateway_diag_task_progress(enum gateway_diag_role role) {
    assert(health[role].alive);
    assert(completed_io); /* must follow a completed positive-byte API call */
    completed_io = 0;
    health[role].progress++;
    health[role].last_progress = ++clock_tick;
}
void gateway_diag_task_stop(enum gateway_diag_role role) {
    assert(health[role].alive);
    health[role].alive = 0;
    health[role].stops++;
    health[role].state = GW_DIAG_STATE_STOPPED;
}
void gateway_diag_event(enum gateway_diag_event event, unsigned long value) { (void)event; (void)value; }
void gateway_diag_boot_event(enum gateway_diag_event event, unsigned long value) { (void)event; (void)value; }
void gateway_diag_add(enum gateway_diag_counter counter, unsigned long value) { counters[counter] += value; }
void gateway_diag_start(void) {}
void gateway_diag_network_ready(void *p) { (void)p; }

void rtw_down_sema(_sema *s) {
    enum operation op = s == &uart_tx_rx_sema ? UART_SEM : TCP_SEM;
    marker(op == UART_SEM ? GW_DIAG_STATE_UART_SEM : GW_DIAG_STATE_TCP_SEM);
    suspend(op);
    if (!s->available) longjmp(escape, 1);
    s->available--;
}
void rtw_up_sema(_sema *s) { assert(!s->available); s->available++; }
void rtw_init_sema(_sema *s, int n) { s->available = n; }
void rtw_free_sema(_sema *s) { (void)s; }
int serial_readable(serial_t *s) { (void)s; return uart_bytes > 0; }
int serial_getc(serial_t *s) {
    (void)s; marker(GW_DIAG_STATE_UART_READ); suspend(GETC);
    assert(!completed_io && uart_bytes > 0); uart_bytes--; completed_io = 1; return 0x41;
}
void serial_putc(serial_t *s, int byte) {
    (void)s; (void)byte; marker(GW_DIAG_STATE_UART_WRITE); suspend(PUTC);
    assert(!completed_io); completed_io = 1;
}
int host_send(int fd, const void *data, size_t size, int flags) {
    (void)fd; (void)data; (void)flags; assert(size > 0);
    marker(GW_DIAG_STATE_TCP_SEND); suspend(SEND);
    assert(!completed_io); if (send_result > 0) completed_io = 1; return send_result;
}
int host_recv(int fd, void *data, size_t size, int flags) {
    (void)fd; assert(size >= 2 && flags == MSG_DONTWAIT);
    marker(GW_DIAG_STATE_TCP_RECV); suspend(RECV);
    assert(!completed_io); if (recv_result > 0) { memset(data, 0x42, (size_t)recv_result); completed_io = 1; }
    return recv_result;
}
int host_getsockopt(int fd, int level, int opt, void *value, void *size) {
    (void)fd; (void)level; (void)opt; (void)size;
    marker(GW_DIAG_STATE_TCPIP); *(int *)value = socket_error; return 0;
}
void vTaskDelete(void *p) {
    assert(!p && !health[current_role].alive);
    assert(health[current_role].stops == 1);
    assert(health[current_role].state == GW_DIAG_STATE_STOPPED);
    ++checks; longjmp(escape, 2);
}
void vTaskDelay(unsigned int ticks) {
    if (ticks == 1000) { marker(GW_DIAG_STATE_WAIT_TASKS); suspend(WAIT_CHILDREN); }
    else if (ticks == 100) { marker(GW_DIAG_STATE_NETWORK_WAIT); suspend(NETWORK_WAIT); }
    else { marker(GW_DIAG_STATE_DELAY); if (ticks == 500) suspend(START_DELAY); else longjmp(escape, 1); }
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned int stack, void *param, unsigned int priority, void *handle) {
    (void)fn; (void)name; (void)stack; (void)param; (void)priority; (void)handle; return pdPASS;
}
int host_socket(int domain, int type, int protocol) { (void)domain; (void)type; (void)protocol; marker(GW_DIAG_STATE_TCPIP); return socket_result; }
int host_bind(int fd, const struct sockaddr *addr, socklen_t size) {
    (void)fd; assert(size == sizeof(struct sockaddr_in)); marker(GW_DIAG_STATE_TCPIP);
    const struct sockaddr_in *a = (const struct sockaddr_in *)addr;
    assert(a->sin_family == AF_INET && a->sin_port == htons(80));
#ifdef PRODUCTION_BRIDGE
    assert(a->sin_addr.s_addr == xnetif[0].ip);
#else
    assert(a->sin_addr.s_addr == INADDR_ANY);
#endif
    ++checks; return bind_result;
}
int host_listen(int fd, int backlog) { (void)fd; assert(backlog == 2); marker(GW_DIAG_STATE_TCPIP); return listen_result; }
int host_accept(int fd, struct sockaddr *addr, void *size) {
    (void)fd; (void)addr; (void)size; marker(GW_DIAG_STATE_ACCEPT); suspend(ACCEPT);
    accept_calls++; assert(accept_calls == 1); return 4;
}
int host_select(int nfds, fd_set *read, fd_set *write, fd_set *err, struct timeval *timeout) {
    (void)nfds; (void)read; (void)write; (void)err;
    assert(timeout->tv_sec == 0 && timeout->tv_usec == 200000);
    marker(accept_calls ? GW_DIAG_STATE_WAIT_TASKS : GW_DIAG_STATE_ACCEPT);
    if (accept_calls) suspend(WAIT_CHILDREN); else suspend(SELECT);
    ++select_calls; return 1;
}
int host_close(int fd) { (void)fd; marker(GW_DIAG_STATE_TCPIP); return 0; }
int wifi_is_ready_to_transceive(int interface) { assert(interface == RTW_STA_INTERFACE); marker(GW_DIAG_STATE_NETWORK_WAIT); return wifi_ready ? RTW_SUCCESS : -1; }
void serial_init(serial_t *s, int tx, int rx) { (void)s; (void)tx; (void)rx; }
void serial_baud(serial_t *s, int baud) { (void)s; (void)baud; }
void serial_format(serial_t *s, int bits, int parity, int stop) { (void)s; (void)bits; (void)parity; (void)stop; }
void serial_rx_fifo_level(serial_t *s, int level) { (void)s; (void)level; }
void serial_set_flow_control(serial_t *s, int mode, int rts, int cts) { (void)s; (void)mode; (void)rts; (void)cts; }
void gpio_init(gpio_t *g, int pin) { (void)g; (void)pin; }
void gpio_dir(gpio_t *g, int dir) { (void)g; (void)dir; }
void gpio_mode(gpio_t *g, int mode) { (void)g; (void)mode; }
void gpio_write(gpio_t *g, int value) { (void)g; (void)value; }

static void reset(void) {
    memset(health, 0, sizeof(health)); memset(counters, 0, sizeof(counters));
    uart_tx_rx_sema.available = tcp_tx_rx_sema.available = 1;
    tx_exit = 0; rx_exit = 1; blocked = NONE; suspend_after = 1; completed_io = 0;
    uart_bytes = 1; send_result = 1; recv_result = 2; socket_error = 0;
    socket_result = 3; bind_result = listen_result = 0;
    select_calls = accept_calls = 0; wifi_ready = 1; xnetif[0].ip = 0x01020304;
    clock_tick = 100;
}
static void run(void (*fn)(void *), enum gateway_diag_role role, int expected_escape, int progress, enum gateway_diag_task_state state) {
    int fd = 4;
    current_role = role;
    int reason = setjmp(escape);
    if (!reason) { fn(&fd); assert(!"task unexpectedly returned"); }
    assert(reason == expected_escape);
    assert(health[role].starts == 1 && health[role].progress == progress);
    assert(health[role].state == state);
    assert(!completed_io); ++checks;
}
static void block_tx(enum operation op, int progress, enum gateway_diag_task_state state) { reset(); blocked = op; run(tx_thread, GW_DIAG_ROLE_TX, 1, progress, state); }
static void block_rx(enum operation op, int progress, enum gateway_diag_task_state state) { reset(); blocked = op; run(rx_thread, GW_DIAG_ROLE_RX, 1, progress, state); }
int main(void) {
    block_tx(UART_SEM, 0, GW_DIAG_STATE_UART_SEM);
    block_tx(GETC, 0, GW_DIAG_STATE_UART_READ);
    block_tx(TCP_SEM, 1, GW_DIAG_STATE_TCP_SEM);
    block_tx(SEND, 1, GW_DIAG_STATE_TCP_SEND);
    reset(); send_result = 0; run(tx_thread, GW_DIAG_ROLE_TX, 2, 1, GW_DIAG_STATE_STOPPED); assert(counters[GW_DIAG_TCP_SEND_ERRORS] == 1);
    reset(); send_result = -1; run(tx_thread, GW_DIAG_ROLE_TX, 2, 1, GW_DIAG_STATE_STOPPED); assert(counters[GW_DIAG_TCP_SEND_ERRORS] == 1);
    reset(); run(tx_thread, GW_DIAG_ROLE_TX, 2, 2, GW_DIAG_STATE_STOPPED); assert(counters[GW_DIAG_TCP_TX_BYTES] == 1);
    reset(); uart_bytes = 2; run(tx_thread, GW_DIAG_ROLE_TX, 2, 3, GW_DIAG_STATE_STOPPED); assert(counters[GW_DIAG_TCP_SHORT_SENDS] == 1);
    reset(); uart_bytes = 0; run(tx_thread, GW_DIAG_ROLE_TX, 2, 0, GW_DIAG_STATE_STOPPED);
    block_rx(TCP_SEM, 0, GW_DIAG_STATE_TCP_SEM);
    block_rx(RECV, 0, GW_DIAG_STATE_TCP_RECV);
    block_rx(UART_SEM, 1, GW_DIAG_STATE_UART_SEM);
    block_rx(PUTC, 1, GW_DIAG_STATE_UART_WRITE);
    reset(); blocked = PUTC; suspend_after = 2; run(rx_thread, GW_DIAG_ROLE_RX, 1, 2, GW_DIAG_STATE_UART_WRITE);
    reset(); recv_result = -1; socket_error = EAGAIN; blocked = RECV; suspend_after = 2; run(rx_thread, GW_DIAG_ROLE_RX, 1, 0, GW_DIAG_STATE_TCP_RECV);
    reset(); recv_result = -1; socket_error = 0; blocked = RECV; suspend_after = 2; run(rx_thread, GW_DIAG_ROLE_RX, 1, 0, GW_DIAG_STATE_TCP_RECV);
    reset(); run(rx_thread, GW_DIAG_ROLE_RX, 1, 3, GW_DIAG_STATE_DELAY); assert(counters[GW_DIAG_UART_TX_BYTES] == 2);
    reset(); recv_result = 0; run(rx_thread, GW_DIAG_ROLE_RX, 2, 0, GW_DIAG_STATE_STOPPED);
    reset(); recv_result = -1; socket_error = EIO; run(rx_thread, GW_DIAG_ROLE_RX, 2, 0, GW_DIAG_STATE_STOPPED); assert(counters[GW_DIAG_TCP_RECV_ERRORS] == 1);
    /* Preserve a suspended RX holding UART sem, then independently execute TX.
     * This models the visible states for hypothetical UART/CTS backpressure;
     * it does not claim to observe a physical CTS level. */
    reset(); blocked = PUTC; run(rx_thread, GW_DIAG_ROLE_RX, 1, 1, GW_DIAG_STATE_UART_WRITE);
    assert(!uart_tx_rx_sema.available); blocked = NONE;
    run(tx_thread, GW_DIAG_ROLE_TX, 1, 0, GW_DIAG_STATE_UART_SEM);
    assert(health[GW_DIAG_ROLE_RX].state == GW_DIAG_STATE_UART_WRITE);
    assert(health[GW_DIAG_ROLE_RX].progress == 1);
    reset(); blocked = START_DELAY; run(example_socket_tcp_trx_thread, GW_DIAG_ROLE_BRIDGE, 1, 0, GW_DIAG_STATE_DELAY);
    reset(); socket_result = -1; run(example_socket_tcp_trx_thread, GW_DIAG_ROLE_BRIDGE, 2, 0, GW_DIAG_STATE_STOPPED);
    reset(); bind_result = -1; run(example_socket_tcp_trx_thread, GW_DIAG_ROLE_BRIDGE, 2, 0, GW_DIAG_STATE_STOPPED);
    reset(); listen_result = -1; run(example_socket_tcp_trx_thread, GW_DIAG_ROLE_BRIDGE, 2, 0, GW_DIAG_STATE_STOPPED);
    reset(); blocked = ACCEPT; run(example_socket_tcp_trx_thread, GW_DIAG_ROLE_BRIDGE, 1, 0, GW_DIAG_STATE_ACCEPT);
    reset(); blocked = WAIT_CHILDREN; run(example_socket_tcp_trx_thread, GW_DIAG_ROLE_BRIDGE, 1, 0, GW_DIAG_STATE_WAIT_TASKS);
#ifdef PRODUCTION_BRIDGE
    reset(); blocked = SELECT; run(example_socket_tcp_trx_thread, GW_DIAG_ROLE_BRIDGE, 1, 0, GW_DIAG_STATE_ACCEPT);
    reset(); wifi_ready = 0; blocked = NETWORK_WAIT; run(example_socket_tcp_trx_thread, GW_DIAG_ROLE_BRIDGE, 1, 0, GW_DIAG_STATE_NETWORK_WAIT);
    reset(); xnetif[0].ip = 0; blocked = NETWORK_WAIT; run(example_socket_tcp_trx_thread, GW_DIAG_ROLE_BRIDGE, 1, 0, GW_DIAG_STATE_NETWORK_WAIT);
#endif
    printf("bridge health source integration: %d assertions passed\n", checks);
    return 0;
}
