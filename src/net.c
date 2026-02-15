#include "net.h"
#include "pci.h"
#include "idt.h"
#include "vga.h"
#include "timer.h"
#include "heap.h"

/* RTL8139 registers */
#define RTL_MAC0     0x00
#define RTL_TXSTAT0  0x10
#define RTL_TXADDR0  0x20
#define RTL_RBSTART  0x30
#define RTL_CMD      0x37
#define RTL_CAPR     0x38
#define RTL_IMR      0x3C
#define RTL_ISR      0x3E
#define RTL_TXCFG    0x40
#define RTL_RXCFG    0x44
#define RTL_CONFIG1  0x52

#define RTL_CMD_RESET 0x10
#define RTL_CMD_RE    0x08
#define RTL_CMD_TE    0x04

#define RX_BUF_SIZE  (8192 + 16 + 1500)
#define TX_BUF_SIZE  1536

/* Network state */
static bool nic_available = false;
static uint16_t io_base = 0;
static mac_addr_t our_mac;
static ip_addr_t our_ip     = {{10, 0, 2, 15}};
static ip_addr_t gateway_ip = {{10, 0, 2, 2}};
static ip_addr_t netmask    = {{255, 255, 255, 0}};
static ip_addr_t dns_server = {{10, 0, 2, 3}};   /* QEMU default DNS */

static uint8_t rx_buffer[RX_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_buffers[4][TX_BUF_SIZE] __attribute__((aligned(4)));
static int current_tx = 0;
static uint32_t rx_offset = 0;

/* ARP cache */
#define ARP_CACHE_SIZE 16
static struct { ip_addr_t ip; mac_addr_t mac; bool valid; } arp_cache[ARP_CACHE_SIZE];

/* Ping state */
static volatile bool ping_received = false;
static volatile uint32_t ping_recv_time = 0;
static uint16_t ping_seq = 0;

/* UDP handler */
static void (*udp_handler)(ip_addr_t, uint16_t, uint16_t, const void*, uint32_t) = NULL;

/* DNS state */
static volatile bool dns_received = false;
static volatile ip_addr_t dns_result = {{0,0,0,0}};
static uint16_t dns_txn_id = 0x1234;

static uint16_t ip_id_counter = 1;

static uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static uint16_t ntohs(uint16_t v) { return htons(v); }

static bool ip_eq(ip_addr_t a, ip_addr_t b) {
    return a.b[0]==b.b[0] && a.b[1]==b.b[1] && a.b[2]==b.b[2] && a.b[3]==b.b[3];
}

static uint16_t ip_checksum(const void* data, uint32_t len) {
    const uint16_t* p = (const uint16_t*)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~(uint16_t)sum;
}

/* RTL8139 driver */
static void rtl_write8(uint16_t reg, uint8_t val)  { outb(io_base + reg, val); }
static void rtl_write16(uint16_t reg, uint16_t val) { outw(io_base + reg, val); }
static void rtl_write32(uint16_t reg, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"((uint16_t)(io_base + reg)));
}
static uint8_t rtl_read8(uint16_t reg) { return inb(io_base + reg); }
static uint16_t rtl_read16(uint16_t reg) { return inw(io_base + reg); }

static void arp_cache_add(ip_addr_t ip, mac_addr_t mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip))
            { arp_cache[i].mac = mac; return; }
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (!arp_cache[i].valid)
            { arp_cache[i].ip = ip; arp_cache[i].mac = mac; arp_cache[i].valid = true; return; }
}

mac_addr_t* net_arp_lookup(ip_addr_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip))
            return &arp_cache[i].mac;
    return NULL;
}

/* Resolve MAC address for an IP (handles gateway routing) */
static mac_addr_t* resolve_mac(ip_addr_t target) {
    /* If target is on same subnet, ARP directly; otherwise use gateway */
    ip_addr_t arp_target = target;
    bool same_subnet = true;
    for (int i = 0; i < 4; i++) {
        if ((target.b[i] & netmask.b[i]) != (our_ip.b[i] & netmask.b[i])) {
            same_subnet = false;
            break;
        }
    }
    if (!same_subnet) arp_target = gateway_ip;

    mac_addr_t* mac = net_arp_lookup(arp_target);
    if (mac) return mac;

    /* Send ARP and wait */
    net_send_arp_request(arp_target);
    uint32_t start = timer_get_ticks();
    while (timer_get_ticks() - start < 200) { net_poll(); hlt(); }
    return net_arp_lookup(arp_target);
}

/* ---- UDP processing ---- */
static void process_udp(ip_addr_t src_ip, const uint8_t* data, uint32_t len) {
    if (len < sizeof(udp_header_t)) return;
    udp_header_t* udp = (udp_header_t*)data;
    uint16_t sport = ntohs(udp->src_port);
    uint16_t dport = ntohs(udp->dst_port);
    uint16_t udp_len = ntohs(udp->length);

    if (udp_len < sizeof(udp_header_t) || udp_len > len) return;
    uint32_t payload_len = udp_len - sizeof(udp_header_t);
    const void* payload = data + sizeof(udp_header_t);

    /* DNS response (from port 53) */
    if (sport == DNS_PORT && payload_len >= sizeof(dns_header_t)) {
        dns_header_t* dns = (dns_header_t*)payload;
        uint16_t id = ntohs(dns->id);
        uint16_t flags = ntohs(dns->flags);
        uint16_t ancount = ntohs(dns->ancount);

        if (id == dns_txn_id && (flags & DNS_FLAG_QR) && ancount > 0) {
            /* Skip question section */
            const uint8_t* p = (const uint8_t*)payload + sizeof(dns_header_t);
            const uint8_t* end = (const uint8_t*)payload + payload_len;

            /* Skip QNAME */
            while (p < end && *p != 0) {
                if ((*p & 0xC0) == 0xC0) { p += 2; goto skip_qtype; }
                p += (*p) + 1;
            }
            p++; /* skip null terminator */
skip_qtype:
            p += 4; /* skip QTYPE + QCLASS */

            /* Parse answers - find first A record */
            for (uint16_t i = 0; i < ancount && p + 12 <= end; i++) {
                /* Name (might be compressed) */
                if ((*p & 0xC0) == 0xC0) p += 2;
                else { while (p < end && *p != 0) p += (*p) + 1; p++; }

                if (p + 10 > end) break;
                uint16_t rtype = (p[0] << 8) | p[1];
                /* uint16_t rclass = (p[2] << 8) | p[3]; */
                /* uint32_t ttl = ...; */
                uint16_t rdlen = (p[8] << 8) | p[9];
                p += 10;

                if (rtype == DNS_TYPE_A && rdlen == 4 && p + 4 <= end) {
                    dns_result.b[0] = p[0];
                    dns_result.b[1] = p[1];
                    dns_result.b[2] = p[2];
                    dns_result.b[3] = p[3];
                    dns_received = true;
                    return;
                }
                p += rdlen;
            }
        }
    }

    /* Call user handler if registered */
    if (udp_handler)
        udp_handler(src_ip, sport, dport, payload, payload_len);
}

/* ---- Packet processing ---- */
static void process_rx_packet(uint8_t* data, uint32_t len) {
    if (len < sizeof(eth_header_t)) return;
    eth_header_t* eth = (eth_header_t*)data;
    uint16_t ethertype = ntohs(eth->ethertype);

    if (ethertype == ETH_TYPE_ARP && len >= sizeof(eth_header_t) + sizeof(arp_packet_t)) {
        arp_packet_t* arp = (arp_packet_t*)(data + sizeof(eth_header_t));
        uint16_t op = ntohs(arp->oper);
        arp_cache_add(arp->spa, arp->sha);
        if (op == ARP_OP_REQUEST && ip_eq(arp->tpa, our_ip)) {
            uint8_t reply[sizeof(eth_header_t) + sizeof(arp_packet_t)];
            eth_header_t* re = (eth_header_t*)reply;
            arp_packet_t* ra = (arp_packet_t*)(reply + sizeof(eth_header_t));
            re->dst = arp->sha; re->src = our_mac;
            re->ethertype = htons(ETH_TYPE_ARP);
            ra->htype = htons(1); ra->ptype = htons(0x0800);
            ra->hlen = 6; ra->plen = 4; ra->oper = htons(ARP_OP_REPLY);
            ra->sha = our_mac; ra->spa = our_ip;
            ra->tha = arp->sha; ra->tpa = arp->spa;
            net_send_raw(reply, sizeof(reply));
        }
    } else if (ethertype == ETH_TYPE_IP && len >= sizeof(eth_header_t) + sizeof(ip_header_t)) {
        ip_header_t* ip = (ip_header_t*)(data + sizeof(eth_header_t));
        uint32_t ip_hdr_len = (ip->ver_ihl & 0x0F) * 4;
        uint32_t ip_total = ntohs(ip->total_len);

        if (ip->protocol == IP_PROTO_ICMP) {
            icmp_header_t* icmp = (icmp_header_t*)(data + sizeof(eth_header_t) + ip_hdr_len);
            if (icmp->type == ICMP_ECHO_REQUEST) {
                uint32_t pkt_size = sizeof(eth_header_t) + ip_total;
                uint8_t reply[1500];
                if (pkt_size > sizeof(reply)) return;
                memcpy(reply, data, pkt_size);
                eth_header_t* re = (eth_header_t*)reply;
                ip_header_t* ri = (ip_header_t*)(reply + sizeof(eth_header_t));
                icmp_header_t* rc = (icmp_header_t*)(reply + sizeof(eth_header_t) + ip_hdr_len);
                re->dst = eth->src; re->src = our_mac;
                ri->dst = ip->src; ri->src = our_ip;
                ri->checksum = 0; ri->checksum = ip_checksum(ri, ip_hdr_len);
                rc->type = ICMP_ECHO_REPLY; rc->checksum = 0;
                rc->checksum = ip_checksum(rc, ip_total - ip_hdr_len);
                net_send_raw(reply, pkt_size);
            } else if (icmp->type == ICMP_ECHO_REPLY) {
                ping_received = true;
                ping_recv_time = timer_get_ticks();
            }
        } else if (ip->protocol == IP_PROTO_UDP) {
            process_udp(ip->src,
                        data + sizeof(eth_header_t) + ip_hdr_len,
                        ip_total - ip_hdr_len);
        }
    }
}

static void rtl_irq_handler(registers_t* regs) {
    (void)regs;
    uint16_t status = rtl_read16(RTL_ISR);
    rtl_write16(RTL_ISR, status);
    if (status & 0x01) {
        while (!(rtl_read8(RTL_CMD) & 0x01)) {
            uint32_t* header = (uint32_t*)(rx_buffer + rx_offset);
            uint32_t rx_status = header[0];
            uint32_t rx_size = (rx_status >> 16) & 0xFFFF;
            if (rx_size == 0 || rx_size > 1500 + 4) break;
            if (!(rx_status & 1)) break;
            process_rx_packet(rx_buffer + rx_offset + 4, rx_size - 4);
            rx_offset = (rx_offset + rx_size + 4 + 3) & ~3;
            if (rx_offset >= 8192) rx_offset -= 8192;
            rtl_write16(RTL_CAPR, rx_offset - 16);
        }
    }
}

void net_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
    pci_device_t* dev = pci_find_device(0x10EC, 0x8139);
    if (!dev) { nic_available = false; return; }

    io_base = dev->bar[0] & 0xFFFC;
    uint8_t irq = dev->irq_line;

    uint32_t cmd = pci_read(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 2);
    pci_write(dev->bus, dev->slot, dev->func, 0x04, cmd);

    rtl_write8(RTL_CONFIG1, 0x00);
    rtl_write8(RTL_CMD, RTL_CMD_RESET);
    int timeout = 1000;
    while ((rtl_read8(RTL_CMD) & RTL_CMD_RESET) && timeout-- > 0);

    for (int i = 0; i < 6; i++) our_mac.b[i] = rtl_read8(RTL_MAC0 + i);
    rtl_write32(RTL_RBSTART, (uint32_t)rx_buffer);
    rtl_write16(RTL_IMR, 0x0005);
    rtl_write32(RTL_RXCFG, 0x0000000F);
    rtl_write8(RTL_CMD, RTL_CMD_RE | RTL_CMD_TE);

    if (irq > 0 && irq < 16) {
        register_interrupt_handler(32 + irq, rtl_irq_handler);
        irq_unmask(irq);
    }
    rx_offset = 0;
    nic_available = true;
}

bool net_is_available(void) { return nic_available; }

void net_send_raw(const void* data, uint32_t len) {
    if (!nic_available || len > TX_BUF_SIZE) return;
    memcpy(tx_buffers[current_tx], data, len);
    if (len < 60) { memset(tx_buffers[current_tx] + len, 0, 60 - len); len = 60; }
    rtl_write32(RTL_TXADDR0 + current_tx * 4, (uint32_t)tx_buffers[current_tx]);
    rtl_write32(RTL_TXSTAT0 + current_tx * 4, len);
    int t = 10000;
    while (t-- > 0) {
        uint32_t stat;
        __asm__ volatile("inl %1, %0" : "=a"(stat) : "Nd"((uint16_t)(io_base + RTL_TXSTAT0 + current_tx * 4)));
        if (stat & 0xC000) break;
    }
    current_tx = (current_tx + 1) % 4;
}

void net_poll(void) {
    if (!nic_available) return;
    uint16_t status = rtl_read16(RTL_ISR);
    if (status) {
        rtl_write16(RTL_ISR, status);
        if (status & 0x01) {
            while (!(rtl_read8(RTL_CMD) & 0x01)) {
                uint32_t* header = (uint32_t*)(rx_buffer + rx_offset);
                uint32_t rx_status = header[0];
                uint32_t rx_size = (rx_status >> 16) & 0xFFFF;
                if (rx_size == 0 || rx_size > 1504 || !(rx_status & 1)) break;
                process_rx_packet(rx_buffer + rx_offset + 4, rx_size - 4);
                rx_offset = (rx_offset + rx_size + 4 + 3) & ~3;
                if (rx_offset >= 8192) rx_offset -= 8192;
                rtl_write16(RTL_CAPR, rx_offset - 16);
            }
        }
    }
}

/* ---- IP configuration ---- */
void net_set_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    our_ip.b[0]=a; our_ip.b[1]=b; our_ip.b[2]=c; our_ip.b[3]=d;
}
void net_set_gateway(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    gateway_ip.b[0]=a; gateway_ip.b[1]=b; gateway_ip.b[2]=c; gateway_ip.b[3]=d;
}
void net_set_netmask(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    netmask.b[0]=a; netmask.b[1]=b; netmask.b[2]=c; netmask.b[3]=d;
}
void net_set_dns(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    dns_server.b[0]=a; dns_server.b[1]=b; dns_server.b[2]=c; dns_server.b[3]=d;
}

ip_addr_t net_get_ip(void)      { return our_ip; }
ip_addr_t net_get_gateway(void) { return gateway_ip; }
ip_addr_t net_get_netmask(void) { return netmask; }
void net_dns_get_server(ip_addr_t* s) { *s = dns_server; }

/* ---- ARP ---- */
void net_send_arp_request(ip_addr_t target_ip) {
    uint8_t pkt[sizeof(eth_header_t) + sizeof(arp_packet_t)];
    eth_header_t* eth = (eth_header_t*)pkt;
    arp_packet_t* arp = (arp_packet_t*)(pkt + sizeof(eth_header_t));
    memset(eth->dst.b, 0xFF, 6);
    eth->src = our_mac; eth->ethertype = htons(ETH_TYPE_ARP);
    arp->htype = htons(1); arp->ptype = htons(0x0800);
    arp->hlen = 6; arp->plen = 4; arp->oper = htons(ARP_OP_REQUEST);
    arp->sha = our_mac; arp->spa = our_ip;
    memset(arp->tha.b, 0, 6); arp->tpa = target_ip;
    net_send_raw(pkt, sizeof(pkt));
}

/* ---- ICMP Ping ---- */
bool net_ping(ip_addr_t target, uint32_t timeout_ms, uint32_t* rtt) {
    if (!nic_available) return false;
    mac_addr_t* dst_mac = resolve_mac(target);
    if (!dst_mac) return false;

    uint8_t pkt[sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(icmp_header_t) + 32];
    eth_header_t* eth = (eth_header_t*)pkt;
    ip_header_t* ip = (ip_header_t*)(pkt + sizeof(eth_header_t));
    icmp_header_t* icmp = (icmp_header_t*)(pkt + sizeof(eth_header_t) + sizeof(ip_header_t));

    eth->dst = *dst_mac; eth->src = our_mac; eth->ethertype = htons(ETH_TYPE_IP);
    ip->ver_ihl = 0x45; ip->tos = 0;
    ip->total_len = htons(sizeof(ip_header_t) + sizeof(icmp_header_t) + 32);
    ip->id = htons(ping_seq); ip->flags_frag = 0; ip->ttl = 64;
    ip->protocol = IP_PROTO_ICMP; ip->checksum = 0;
    ip->src = our_ip; ip->dst = target;
    ip->checksum = ip_checksum(ip, sizeof(ip_header_t));

    icmp->type = ICMP_ECHO_REQUEST; icmp->code = 0;
    icmp->id = htons(0x1234); icmp->seq = htons(ping_seq++); icmp->checksum = 0;
    uint8_t* payload = (uint8_t*)(icmp + 1);
    for (int i = 0; i < 32; i++) payload[i] = i;
    icmp->checksum = ip_checksum(icmp, sizeof(icmp_header_t) + 32);

    ping_received = false;
    uint32_t send_time = timer_get_ticks();
    net_send_raw(pkt, sizeof(pkt));

    uint32_t deadline = send_time + (timeout_ms * 100) / 1000;
    while (timer_get_ticks() < deadline) {
        net_poll();
        if (ping_received) {
            if (rtt) *rtt = (ping_recv_time - send_time) * 10;
            return true;
        }
        hlt();
    }
    return false;
}

/* ---- UDP send ---- */
bool net_send_udp(ip_addr_t dst_ip, uint16_t src_port, uint16_t dst_port,
                  const void* data, uint32_t len) {
    if (!nic_available) return false;
    if (len > 1400) return false;

    mac_addr_t* dst_mac = resolve_mac(dst_ip);
    if (!dst_mac) return false;

    uint32_t total_len = sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t) + len;
    uint8_t pkt[1500];
    if (total_len > sizeof(pkt)) return false;

    eth_header_t* eth = (eth_header_t*)pkt;
    ip_header_t*  ip  = (ip_header_t*)(pkt + sizeof(eth_header_t));
    udp_header_t* udp = (udp_header_t*)(pkt + sizeof(eth_header_t) + sizeof(ip_header_t));
    uint8_t* payload   = pkt + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t);

    /* Ethernet */
    eth->dst = *dst_mac;
    eth->src = our_mac;
    eth->ethertype = htons(ETH_TYPE_IP);

    /* IP */
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(sizeof(ip_header_t) + sizeof(udp_header_t) + len);
    ip->id = htons(ip_id_counter++);
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->checksum = 0;
    ip->src = our_ip;
    ip->dst = dst_ip;
    ip->checksum = ip_checksum(ip, sizeof(ip_header_t));

    /* UDP */
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(sizeof(udp_header_t) + len);
    udp->checksum = 0; /* Optional for IPv4 */

    /* Payload */
    memcpy(payload, data, len);

    net_send_raw(pkt, total_len);
    return true;
}

void net_udp_set_handler(void (*handler)(ip_addr_t, uint16_t, uint16_t, const void*, uint32_t)) {
    udp_handler = handler;
}

/* ---- DNS resolver ---- */

/* Encode hostname into DNS wire format (e.g. "google.com" -> "\x06google\x03com\x00") */
static int dns_encode_name(const char* name, uint8_t* out, int max) {
    int pos = 0;
    const char* p = name;
    while (*p) {
        const char* dot = p;
        while (*dot && *dot != '.') dot++;
        int label_len = dot - p;
        if (label_len > 63 || pos + label_len + 1 >= max) return -1;
        out[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++) out[pos++] = p[i];
        p = (*dot) ? dot + 1 : dot;
    }
    if (pos < max) out[pos++] = 0; /* Null terminator */
    return pos;
}

bool net_dns_resolve(const char* hostname, ip_addr_t* result, uint32_t timeout_ms) {
    if (!nic_available || !hostname || !result) return false;

    /* Check if it's already an IP address (simple check) */
    if (isdigit(hostname[0])) {
        int a=0, b=0, c=0, d=0;
        const char* p = hostname;
        a = atoi(p); while (*p && *p != '.') p++; if (*p) p++;
        b = atoi(p); while (*p && *p != '.') p++; if (*p) p++;
        c = atoi(p); while (*p && *p != '.') p++; if (*p) p++;
        d = atoi(p);
        if (a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
            c >= 0 && c <= 255 && d >= 0 && d <= 255) {
            result->b[0] = a; result->b[1] = b;
            result->b[2] = c; result->b[3] = d;
            return true;
        }
    }

    /* Build DNS query */
    uint8_t query[512];
    int pos = 0;

    /* DNS header */
    dns_txn_id++;
    dns_header_t* dns = (dns_header_t*)query;
    dns->id = htons(dns_txn_id);
    dns->flags = htons(DNS_FLAG_RD);  /* Recursion desired */
    dns->qdcount = htons(1);
    dns->ancount = 0;
    dns->nscount = 0;
    dns->arcount = 0;
    pos = sizeof(dns_header_t);

    /* Question section: QNAME + QTYPE + QCLASS */
    int name_len = dns_encode_name(hostname, query + pos, sizeof(query) - pos - 4);
    if (name_len < 0) return false;
    pos += name_len;

    /* QTYPE = A (1), QCLASS = IN (1) */
    query[pos++] = 0; query[pos++] = DNS_TYPE_A;
    query[pos++] = 0; query[pos++] = DNS_CLASS_IN;

    /* Send to DNS server */
    dns_received = false;
    net_send_udp(dns_server, 1024 + (dns_txn_id & 0xFF), DNS_PORT, query, pos);

    /* Wait for response */
    uint32_t start = timer_get_ticks();
    uint32_t deadline = start + (timeout_ms * 100) / 1000;
    while (timer_get_ticks() < deadline) {
        net_poll();
        if (dns_received) {
            *result = dns_result;
            return true;
        }
        hlt();
    }
    return false;
}

/* ---- Display functions ---- */
void net_ifconfig(void) {
    if (!nic_available) {
        kprintf("  No network interface detected.\n");
        kprintf("  Try: qemu-system-i386 -kernel microkernel.bin -m 128M -netdev user,id=n0 -device rtl8139,netdev=n0\n");
        return;
    }
    kprintf("  eth0:\n");
    kprintf("    MAC:     %d:%d:%d:%d:%d:%d\n",
            our_mac.b[0], our_mac.b[1], our_mac.b[2],
            our_mac.b[3], our_mac.b[4], our_mac.b[5]);
    kprintf("    IP:      %d.%d.%d.%d\n", our_ip.b[0], our_ip.b[1], our_ip.b[2], our_ip.b[3]);
    kprintf("    Gateway: %d.%d.%d.%d\n", gateway_ip.b[0], gateway_ip.b[1], gateway_ip.b[2], gateway_ip.b[3]);
    kprintf("    Netmask: %d.%d.%d.%d\n", netmask.b[0], netmask.b[1], netmask.b[2], netmask.b[3]);
    kprintf("    DNS:     %d.%d.%d.%d\n", dns_server.b[0], dns_server.b[1], dns_server.b[2], dns_server.b[3]);
    kprintf("    Driver:  RTL8139 (IO %x)\n", io_base);
}

void net_arp_table(void) {
    kprintf("  IP Address        MAC Address\n");
    kprintf("  ---------------   -----------------\n");
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid) {
            kprintf("  %d.%d.%d.%d",
                    arp_cache[i].ip.b[0], arp_cache[i].ip.b[1],
                    arp_cache[i].ip.b[2], arp_cache[i].ip.b[3]);
            kprintf("       ");
            kprintf("%d:%d:%d:%d:%d:%d\n",
                    arp_cache[i].mac.b[0], arp_cache[i].mac.b[1], arp_cache[i].mac.b[2],
                    arp_cache[i].mac.b[3], arp_cache[i].mac.b[4], arp_cache[i].mac.b[5]);
        }
    }
}
