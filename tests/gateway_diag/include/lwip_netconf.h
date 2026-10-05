#ifndef TEST_LWIP_NETCONF_H
#define TEST_LWIP_NETCONF_H
#include <stdint.h>
#define NET_IF_NUM 2
struct netif { unsigned int flags; uint32_t ip_addr; };
unsigned int test_netif_flags(const struct netif *netif);
uint32_t test_netif_ip(const uint32_t *ip);
#define netif_is_up(n) (test_netif_flags(n) & 1U)
#define netif_is_link_up(n) (test_netif_flags(n) & 2U)
#define netif_ip_addr4(n) (&(n)->ip_addr)
#define ip_addr_get_ip4_u32(ip) test_netif_ip(ip)
#endif
