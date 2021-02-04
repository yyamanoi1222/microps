#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "util.h"
#include "net.h"
#include "ether.h"

#include "ether_pcap.h"

struct ether_pcap {
    char name[IFNAMSIZ];
    int fd;
};

#define PRIV(x) ((struct ether_pcap *)x->priv)

static int
ether_pcap_addr(struct net_device *dev) {
    int soc;
    struct ifreq ifr = {};

    soc = socket(AF_INET, SOCK_DGRAM, 0);
    if (soc == -1) {
        errorf("socket: %s, dev=%s", strerror(errno), dev->name);
        return -1;
    }
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, PRIV(dev)->name, sizeof(ifr.ifr_name)-1);
    if (ioctl(soc, SIOCGIFHWADDR, &ifr) == -1) {
        errorf("ioctl [SIOCGIFHWADDR]: %s, dev=%s", strerror(errno), dev->name);
        close(soc);
        return -1;
    }
    memcpy(dev->addr, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);
    close(soc);
    return 0;
}

static int
ether_pcap_open(struct net_device *dev)
{
    struct ether_pcap *pcap;
    struct sockaddr_ll addr = {};
    struct ifreq ifr = {};

    pcap = PRIV(dev);

    pcap->fd = socket(PF_PACKET, SOCK_RAW, hton16(ETH_P_ALL));
    if (pcap->fd == -1) {
        errorf("socket: %s, dev=%s", strerror(errno), dev->name);
        return -1;
    }
    strncpy(ifr.ifr_name, pcap->name, sizeof(ifr.ifr_name)-1);
    if (ioctl(pcap->fd, SIOCGIFINDEX, &ifr) == -1) {
        errorf("ioctl [SIOCGIFINDEX]: %s, dev=%s", strerror(errno), dev->name);
        close(pcap->fd);
        return -1;
    }
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = hton16(ETH_P_ALL);
    addr.sll_ifindex = ifr.ifr_ifindex;
    if (bind(pcap->fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        errorf("bind: %s, dev=%s", strerror(errno), dev->name);
        close(pcap->fd);
        return -1;
    }
    if (ioctl(pcap->fd, SIOCGIFFLAGS, &ifr) == -1) {
        errorf("ioctl [SIOCGIFFLAGS]: %s, dev=%s", strerror(errno), dev->name);
        close(pcap->fd);
        return -1;
    }
    ifr.ifr_flags = ifr.ifr_flags | IFF_PROMISC;
    if (ioctl(pcap->fd, SIOCSIFFLAGS, &ifr) == -1) {
        errorf("ioctl [SIOCSIFFLAGS]: %s, dev=%s", strerror(errno), dev->name);
        close(pcap->fd);
        return -1;
    }
    if (memcmp(dev->addr, ETHER_ADDR_ANY, ETHER_ADDR_LEN) == 0) {
        if (ether_pcap_addr(dev) == -1) {
            errorf("ether_pcap_addr() failure, dev=%s", dev->name);
            close(pcap->fd);
            return -1;
        }
    }
    return 0;
};

static int
ether_pcap_close(struct net_device *dev)
{
    close(PRIV(dev)->fd);
    return 0;
}

static ssize_t
ether_pcap_write(struct net_device *dev, const uint8_t *frame, size_t flen)
{
    return write(PRIV(dev)->fd, frame, flen);
}

int
ether_pcap_transmit(struct net_device *dev, uint16_t type, const uint8_t *buf, size_t len, const void *dst)
{
    return ether_transmit_helper(dev, type, buf, len, dst, ether_pcap_write);
}

static ssize_t
ether_pcap_read(struct net_device *dev, uint8_t *buf, size_t size)
{
    ssize_t len;

    len = read(PRIV(dev)->fd, buf, size);
    if (len <= 0) {
        if (len == -1 && errno != EINTR) {
            errorf("read: %s, dev=%s", strerror(errno), dev->name);
        }
        return -1;
    }
    return len;
}

static int
ether_pcap_poll(struct net_device *dev)
{
    struct pollfd pfd;
    int ret;

    pfd.fd = PRIV(dev)->fd;
    pfd.events = POLLIN;
    ret = poll(&pfd, 1, 0);
    switch (ret) {
    case -1:
        if (errno != EINTR) {
            errorf("poll: %s, dev=%s", strerror(errno), dev->name);
        }
        /* fall through */
    case 0:
        return -1;
    }
    return ether_poll_helper(dev, ether_pcap_read);
}

static struct net_device_ops ether_pcap_ops = {
    .open = ether_pcap_open,
    .close = ether_pcap_close,
    .transmit = ether_pcap_transmit,
    .poll = ether_pcap_poll,
};

struct net_device *
ether_pcap_init(const char *name, const char *addr)
{
    struct net_device *dev;
    struct ether_pcap *pcap;

    dev = net_device_alloc(ether_setup_helper);
    if (!dev) {
        errorf("net_device_alloc() failure");
        return NULL;
    }
    if (addr) {
        if (ether_addr_pton(addr, dev->addr) == -1) {
            errorf("invalid address, addr=%s", addr);
            return NULL;
        }
    }
    dev->ops = &ether_pcap_ops;
    pcap = calloc(1, sizeof(*pcap));
    if (!pcap) {
        errorf("calloc() failure");
        return NULL;
    }
    strncpy(pcap->name, name, sizeof(pcap->name)-1);
    pcap->fd = -1;
    dev->priv = pcap;
    if (net_device_register(dev) == -1) {
        errorf("net_device_register() failure");
        free(pcap);
        return NULL;
    }
    debugf("ethernet device initialized, dev=%s", dev->name);
    return dev;
}
