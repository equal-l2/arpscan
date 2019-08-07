#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring> // memcpy
#include <optional>
#include <thread>

#ifdef __linux__
#   include <linux/if_packet.h>
#   include <linux/if_ether.h>
#else
#   include <net/bpf.h>
#endif

#include "types.h"
#include "util.h"

// 一定時間ソケットを読んでARPレスポンスがあれば表示する
int accept_arp_for(unsigned ms, int fd, uint8_t* buf, size_t buf_len, ether_addr my_haddr) {
    const auto start = std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-start < std::chrono::milliseconds(ms)) {
        const auto ret = read_arp_resp(fd, buf, buf_len);
        if (!ret.has_value()) {
            return -1;
        }
        for(arp a : *ret) {
            if (a.t_ha == my_haddr) {
                printf("%s : %s\n", format_haddr(a.s_ha).data(), format_paddr(a.s_pa).data());
            }/* else {
                printf("Wrong %s : %s\n", format_haddr(a.s_ha).data(), format_paddr(a.s_pa).data());
                }*/
        }
    }
    return 0;
}

#if defined(__linux__) || defined(__sun)
struct sockaddr_ll configure_sockaddr(const char* ifname, int fd, ether_addr haddr) {
    struct ifreq ifr;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) == -1) {
        perror("ioctl");
        return -1;
    }
    struct sockaddr_ll sendaddr;
    sendaddr.sll_family = AF_PACKET;
    sendaddr.sll_protocol = htons(ETH_P_ARP);
    sendaddr.sll_ifindex = ifr.ifr_ifindex;
    sendaddr.sll_hatype = 0;
    sendaddr.sll_pkttype = 0;
    sendaddr.sll_halen = ETHER_ADDR_LEN;
    memcpy(sendaddr.sll_addr, OCTET(haddr), ETHER_ADDR_LEN);
}
#endif

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "%s [interface]\n", argv[0]);
        return -1;
    }

    int fd;
    if ((fd = sock_open(argv[1]) ) == -1) {
        return -1;
    }

#if defined(__linux__) || defined(__sun)
    unsigned buf_len = 4096;
#else
    // バッファ長取得
    // BPFは指定された長さのバッファを使わなければならない
    unsigned buf_len;
    if (ioctl(fd, BIOCGBLEN, &buf_len) == -1) {
        perror("ioctl");
        return -1;
    }
#endif

    // 自身のMACアドレスとIPアドレスを取得する
    const auto ap_opt = get_addr_pair(argv[1]);
    if (!ap_opt) {
        fprintf(stderr, "Some addresses are not assigned to \"%s\"\n", argv[1]);
        return -1;
    }
    const auto ap = *ap_opt;

    printf("Host MAC address : %s\n", format_haddr(ap.haddr).data());
#if defined(__linux__) || defined(__sun)
    // sendtoで使うsockaddr構造体を用意する
    const struct sockaddr_ll sendaddr = configure_sockaddr(argv[1], fd, ap.haddr);
#endif
    printf("[*] Start sending ARP requests as %s\n", format_paddr(ap.paddr).data());

    const auto paddr = ap.paddr.s_addr;
    const auto mask = ap.mask.s_addr;

    const auto netaddr = paddr & mask;
    const auto bcastaddr = paddr | (~mask);

    const auto begin = ntohl(netaddr)+1;
    const auto end = ntohl(bcastaddr);
    printf(
            "[*] Send to IP between %s and %s (%d host(s))\n",
            format_paddr({htonl(begin)}).data(),
            format_paddr({htonl(end-1)}).data(),
            end-begin
          );

    auto buf = new uint8_t[buf_len];

    for(uint32_t i = begin; i < end; i++) {
        const in_addr addr = {htonl(i)};
#if defined(__linux__) || defined(__sun)
        sendto(fd, generate_arp_frame(ap.haddr, ap.paddr, addr).data(), 42, 0, (const struct sockaddr*)&sendaddr, sizeof(sendaddr));
#else
        write(fd, generate_arp_frame(ap.haddr, ap.paddr, addr).data(), 42);
#endif
        accept_arp_for(10, fd, buf, buf_len, ap.haddr);
    }

    puts("[*] ARP requests sent, waiting replies for 3 seconds...");
    accept_arp_for(3000, fd, buf, buf_len, ap.haddr);
    close(fd);
}
