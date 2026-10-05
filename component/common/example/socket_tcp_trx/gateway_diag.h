#ifndef GATEWAY_DIAG_H
#define GATEWAY_DIAG_H

/* No strings or payloads accepted: diagnostics must not
 * contain Wi-Fi credentials, Zigbee traffic, or arbitrary SDK console text. */
enum gateway_diag_event {
    GW_DIAG_APP_START,
    GW_DIAG_NETWORK_START,
    GW_DIAG_UART_READY,
    GW_DIAG_BRIDGE_START,
    GW_DIAG_BRIDGE_LISTEN,
    GW_DIAG_BRIDGE_CONNECT,
    GW_DIAG_BRIDGE_DISCONNECT,
    GW_DIAG_BRIDGE_ERROR,
    GW_DIAG_TX_EXIT,
    GW_DIAG_RX_EXIT,
    GW_DIAG_TASK_ERROR,
    GW_DIAG_NETWORK_STATE,
    GW_DIAG_LOG_LISTEN,
    GW_DIAG_LOG_ERROR,
    GW_DIAG_NETWORK_READY,
    GW_DIAG_EVENT_COUNT
};

enum gateway_diag_counter {
    GW_DIAG_UART_RX_BYTES,
    GW_DIAG_TCP_TX_BYTES,
    GW_DIAG_TCP_RX_BYTES,
    GW_DIAG_UART_TX_BYTES,
    GW_DIAG_TCP_SEND_ERRORS,
    GW_DIAG_TCP_RECV_ERRORS,
    GW_DIAG_TCP_SHORT_SENDS,
    GW_DIAG_UART_BUFFER_DROPS,
    GW_DIAG_BRIDGE_CONNECTIONS,
    GW_DIAG_BRIDGE_DISCONNECTS,
    GW_DIAG_COUNTER_COUNT
};

enum gateway_diag_role {
    GW_DIAG_ROLE_TX,
    GW_DIAG_ROLE_RX,
    GW_DIAG_ROLE_BRIDGE,
    GW_DIAG_ROLE_DIAG,
    GW_DIAG_ROLE_COUNT
};

enum gateway_diag_task_state {
    GW_DIAG_STATE_STOPPED,
    GW_DIAG_STATE_IDLE,
    GW_DIAG_STATE_UART_SEM,
    GW_DIAG_STATE_UART_READ,
    GW_DIAG_STATE_UART_WRITE,
    GW_DIAG_STATE_TCP_SEM,
    GW_DIAG_STATE_TCP_SEND,
    GW_DIAG_STATE_TCP_RECV,
    GW_DIAG_STATE_ACCEPT,
    GW_DIAG_STATE_WAIT_TASKS,
    GW_DIAG_STATE_NETWORK_WAIT,
    GW_DIAG_STATE_DELAY,
    GW_DIAG_STATE_TCPIP,
    GW_DIAG_STATE_COUNT
};

/* Self-report only, in the corresponding HP task. No TaskHandle is retained.
 * start/stop bracket task lifetime; stop must precede vTaskDelete(NULL).
 * heartbeat records loop execution at most once per second and measures the
 * current task's stack outside the critical section at most once per 5 seconds.
 * progress records only successful byte I/O; heartbeat/progress do not prove
 * that the radio, CTS pin or the remote endpoint is healthy. State markers
 * describe the next API operation, including waits which may never return. */
void gateway_diag_task_start(enum gateway_diag_role role);
void gateway_diag_task_state(enum gateway_diag_role role,
                             enum gateway_diag_task_state state);
void gateway_diag_task_heartbeat(enum gateway_diag_role role);
void gateway_diag_task_progress(enum gateway_diag_role role);
void gateway_diag_task_stop(enum gateway_diag_role role);

/* Single-writer HP startup only, before vTaskStartScheduler. No RTOS calls. */
void gateway_diag_boot_event(enum gateway_diag_event event, unsigned long value);
/* HP task context only, after the scheduler starts. Never use from an ISR. */
void gateway_diag_event(enum gateway_diag_event event, unsigned long value);
void gateway_diag_add(enum gateway_diag_counter counter, unsigned long value);
/* TCPIP initialization completion callback, after network interfaces exist. */
void gateway_diag_network_ready(void *unused);
/* Called once from HP startup, before vTaskStartScheduler. */
void gateway_diag_start(void);

#endif
