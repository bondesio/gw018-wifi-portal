#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <unistd.h>
int test_socket(int domain, int type, int protocol);
int test_ioctl(int fd, unsigned long request, ...);
int test_bind(int fd, const struct sockaddr *address, socklen_t size);
int test_listen(int fd, int backlog);
int test_accept(int fd, struct sockaddr *address, socklen_t *size);
int test_close(int fd);
int test_getsockopt(int fd, int level, int option, void *value, socklen_t *size);
ssize_t test_send(int fd, const void *data, size_t size, int flags);
ssize_t test_recv(int fd, void *data, size_t size, int flags);
#define socket test_socket
#define ioctl test_ioctl
#define bind test_bind
#define listen test_listen
#define accept test_accept
#define close test_close
#define getsockopt test_getsockopt
#define send test_send
#define recv test_recv

int lwip_abortclose(int fd);
int lwip_diag_can_send(int fd);
