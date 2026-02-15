#ifndef NET_H
#define NET_H

#include "types.h"

#define ETH_ALEN 6
#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IP   0x0800

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

/* DNS */
#define DNS_PORT       53
#define DNS_MAX_NAME   128
#define DNS_TYPE_A     1
#define DNS_CLASS_IN   1
#define DNS_FLAG_RD    0x0100   /* Recursion desired */
#define DNS_FLAG_QR    0x8000   /* Response flag */

typedef struct { uint8_t b[4]; } ip_addr_t;
typedef struct { uint8_t b[6]; } mac_addr_t;

typedef struct __attribute__((packed)) {
    mac_addr_t dst, src;
    uint16_t ethertype;
} eth_header_t;

typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    mac_addr_t sha;
    ip_addr_t  spa;
    mac_addr_t tha;
    ip_addr_t  tpa;
} arp_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    ip_addr_t src, dst;
} ip_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, code;
    uint16_t checksum;
    uint16_t id, seq;
} icmp_header_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_header_t;

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

/* Network interface */
void    net_init(void);
bool    net_is_available(void);
void    net_send_raw(const void* data, uint32_t len);
void    net_poll(void);

/* IP config */
void    net_set_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void    net_set_gateway(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void    net_set_netmask(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void    net_set_dns(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

/* High-level */
void    net_send_arp_request(ip_addr_t target_ip);
bool    net_ping(ip_addr_t target, uint32_t timeout_ms, uint32_t* rtt);
void    net_ifconfig(void);
void    net_arp_table(void);
mac_addr_t* net_arp_lookup(ip_addr_t ip);

/* UDP */
bool    net_send_udp(ip_addr_t dst_ip, uint16_t src_port, uint16_t dst_port,
                     const void* data, uint32_t len);
void    net_udp_set_handler(void (*handler)(ip_addr_t src, uint16_t sport,
                            uint16_t dport, const void* data, uint32_t len));

/* DNS */
bool    net_dns_resolve(const char* hostname, ip_addr_t* result, uint32_t timeout_ms);
void    net_dns_get_server(ip_addr_t* server);

/* Info for /proc */
ip_addr_t net_get_ip(void);
ip_addr_t net_get_gateway(void);
ip_addr_t net_get_netmask(void);

#endif
