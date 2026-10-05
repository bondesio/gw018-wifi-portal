#ifndef HOST_PLATFORM_H
#define HOST_PLATFORM_H
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#define LWIP_VERSION_MAJOR 2
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define pdMS_TO_TICKS(n) (n)
#define _PA_18 18
#define _PA_19 19
#define _PA_16 16
#define _PA_17 17
#define _PB_22 22
#define PIN_OUTPUT 1
#define PullNone 0
#define ParityNone 0
#define FifoLvHalf 0
#define FlowControlRTSCTS 1
#define NET_IF_NUM 1
#define RTW_STA_INTERFACE 0
#define RTW_SUCCESS 0
typedef struct { int uart_idx; } serial_t;
typedef int gpio_t;
typedef struct { int available; } _sema;
struct netif { uint32_t ip; };
#define netif_ip_addr4(n) (&(n)->ip)
#define ip_addr_get_ip4_u32(p) (*(p))
void rtw_down_sema(_sema *s);
void rtw_up_sema(_sema *s);
void rtw_init_sema(_sema *s, int n);
void rtw_free_sema(_sema *s);
void vTaskDelete(void *p);
void vTaskDelay(unsigned int ticks);
int xTaskCreate(void (*fn)(void *), const char *name, unsigned int stack, void *param, unsigned int priority, void *handle);
int serial_readable(serial_t *s);
int serial_getc(serial_t *s);
void serial_putc(serial_t *s, int byte);
void serial_init(serial_t *s, int tx, int rx);
void serial_baud(serial_t *s, int baud);
void serial_format(serial_t *s, int bits, int parity, int stop);
void serial_rx_fifo_level(serial_t *s, int level);
void serial_set_flow_control(serial_t *s, int mode, int rts, int cts);
void gpio_init(gpio_t *g, int pin);
void gpio_dir(gpio_t *g, int dir);
void gpio_mode(gpio_t *g, int mode);
void gpio_write(gpio_t *g, int value);
int wifi_is_ready_to_transceive(int interface);
int host_socket(int domain, int type, int protocol);
int host_bind(int fd, const struct sockaddr *addr, socklen_t size);
int host_listen(int fd, int backlog);
int host_accept(int fd, struct sockaddr *addr, void *size);
int host_select(int nfds, fd_set *read, fd_set *write, fd_set *err, struct timeval *timeout);
int host_send(int fd, const void *data, size_t size, int flags);
int host_recv(int fd, void *data, size_t size, int flags);
int host_getsockopt(int fd, int level, int opt, void *value, void *size);
int host_close(int fd);
#define socket host_socket
#define bind host_bind
#define listen host_listen
#define accept host_accept
#define select host_select
#define send host_send
#define recv host_recv
#define getsockopt host_getsockopt
#define close host_close
#endif
