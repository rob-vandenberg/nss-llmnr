/*
 * nss-llmnr - glibc NSS module that resolves single-label host names
 * via LLMNR (RFC 4795) the way Windows does:
 *
 *   1. send the LLMNR query on every active IPv4 interface
 *   2. wait and collect ALL answers from all responders
 *      (instead of taking only the first one, as systemd-resolved does)
 *   3. sort the addresses: addresses in one of our own subnets first,
 *      then private (RFC 1918) addresses, then everything else
 *
 * This fixes lookups of multi-homed Windows hosts, which answer from every
 * NIC. Without this, Linux picks whichever answer arrives first.
 *
 * Usage in /etc/nsswitch.conf (before "dns"):
 *   hosts: files llmnr mdns4_minimal [NOTFOUND=return] dns
 *
 * Copyright (C) 2026 Rob Vandenberg
 * License: AGPL-3.0-or-later (see LICENSE)
 */

#define _GNU_SOURCE
#include <nss.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/random.h>

#define LLMNR_PORT        5355
#define LLMNR_GROUP       "224.0.0.252"
#define MAX_IFACES        32
#define MAX_ADDRS         32
#define MAX_TRIES         3      /* RFC 4795 2.7: at most three transmissions */
#define FIRST_WAIT_MS     300    /* wait per try for a first answer           */
#define COLLECT_MS        250    /* after the first answer, keep collecting   */
#define MAX_NAME          255

struct iface {
    struct in_addr addr;
    struct in_addr mask;
};

struct result {
    struct in_addr addr[MAX_ADDRS];
    int n;
};

/* ------------------------------------------------------------------ */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int get_ifaces(struct iface *ifs)
{
    struct ifaddrs *ifa, *p;
    int n = 0;

    if (getifaddrs(&ifa) != 0)
        return 0;
    for (p = ifa; p && n < MAX_IFACES; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_netmask)
            continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK))
            continue;
        if (!(p->ifa_flags & IFF_MULTICAST))
            continue;
        ifs[n].addr = ((struct sockaddr_in *)p->ifa_addr)->sin_addr;
        ifs[n].mask = ((struct sockaddr_in *)p->ifa_netmask)->sin_addr;
        n++;
    }
    freeifaddrs(ifa);
    return n;
}

/* Single label: no dots, except an optional trailing one. */
static int normalise_name(const char *in, char *out)
{
    size_t len = strlen(in);

    if (len == 0 || len > 63 + 1)
        return -1;
    if (in[len - 1] == '.')
        len--;
    if (len == 0 || len > 63)
        return -1;
    if (memchr(in, '.', len))
        return -1;
    memcpy(out, in, len);
    out[len] = '\0';
    return 0;
}

static int build_query(uint8_t *buf, uint16_t id, const char *label)
{
    size_t l = strlen(label);
    int p = 0;

    memset(buf, 0, 12);
    buf[0] = id >> 8;
    buf[1] = id & 0xff;
    /* flags: all zero (standard query, C/TC/T clear) */
    buf[5] = 1;                 /* QDCOUNT = 1 */
    p = 12;
    buf[p++] = (uint8_t)l;
    memcpy(buf + p, label, l);
    p += l;
    buf[p++] = 0;               /* root */
    buf[p++] = 0; buf[p++] = 1; /* QTYPE  A  */
    buf[p++] = 0; buf[p++] = 1; /* QCLASS IN */
    return p;
}

/* Skip a (possibly compressed) domain name. Returns new offset or -1. */
static int skip_name(const uint8_t *m, int len, int off)
{
    int guard = 0;

    while (off < len && guard++ < 128) {
        uint8_t c = m[off];
        if (c == 0)
            return off + 1;
        if ((c & 0xc0) == 0xc0)
            return (off + 1 < len) ? off + 2 : -1;
        if (c & 0xc0)
            return -1;
        off += 1 + c;
    }
    return -1;
}

static int is_own_addr(struct in_addr a, const struct iface *ifs, int nif)
{
    for (int i = 0; i < nif; i++)
        if (ifs[i].addr.s_addr == a.s_addr)
            return 1;
    return 0;
}

static void add_addr(struct result *r, struct in_addr a)
{
    for (int i = 0; i < r->n; i++)
        if (r->addr[i].s_addr == a.s_addr)
            return;
    if (r->n < MAX_ADDRS)
        r->addr[r->n++] = a;
}

/* Validate an LLMNR response and harvest its A records. Returns 1 if valid. */
static int parse_response(const uint8_t *m, int len, uint16_t id,
                          const char *label, struct result *r)
{
    int off, qd, an;
    size_t l = strlen(label);

    if (len < 12)
        return 0;
    if (((m[0] << 8) | m[1]) != id)
        return 0;
    if (!(m[2] & 0x80))                 /* QR must be 1            */
        return 0;
    if ((m[2] >> 3) & 0x0f)             /* opcode must be 0        */
        return 0;
    if (m[2] & 0x01)                    /* T (tentative): discard  */
        return 0;
    if (m[3] & 0x0f)                    /* RCODE must be 0         */
        return 0;
    qd = (m[4] << 8) | m[5];
    an = (m[6] << 8) | m[7];
    if (qd != 1)
        return 0;

    /* question must be our name */
    off = 12;
    if (off + 1 + (int)l + 1 + 4 > len || m[off] != l)
        return 0;
    if (strncasecmp((const char *)m + off + 1, label, l) != 0 || m[off + 1 + l] != 0)
        return 0;
    off += 1 + l + 1 + 4;

    for (int i = 0; i < an; i++) {
        int type, class, rdlen;

        off = skip_name(m, len, off);
        if (off < 0 || off + 10 > len)
            break;
        type  = (m[off] << 8) | m[off + 1];
        class = ((m[off + 2] << 8) | m[off + 3]) & 0x7fff;
        rdlen = (m[off + 8] << 8) | m[off + 9];
        off += 10;
        if (off + rdlen > len)
            break;
        if (type == 1 && class == 1 && rdlen == 4) {
            struct in_addr a;
            memcpy(&a.s_addr, m + off, 4);
            add_addr(r, a);
        }
        off += rdlen;
    }
    return 1;
}

static int rank(struct in_addr a, const struct iface *ifs, int nif)
{
    uint32_t h = ntohl(a.s_addr);

    for (int i = 0; i < nif; i++)
        if ((a.s_addr & ifs[i].mask.s_addr) == (ifs[i].addr.s_addr & ifs[i].mask.s_addr))
            return 0;                                   /* on our own subnet */
    if ((h >> 24) == 10 || (h >> 20) == 0xac1 || (h >> 16) == 0xc0a8)
        return 1;                                       /* RFC 1918 private  */
    if ((h >> 16) == 0xa9fe)
        return 3;                                       /* 169.254 link-local */
    return 2;                                           /* anything else      */
}

static void sort_result(struct result *r, const struct iface *ifs, int nif)
{
    /* stable insertion sort on rank: keeps arrival order within a rank */
    for (int i = 1; i < r->n; i++) {
        struct in_addr a = r->addr[i];
        int ra = rank(a, ifs, nif), j = i - 1;
        while (j >= 0 && rank(r->addr[j], ifs, nif) > ra) {
            r->addr[j + 1] = r->addr[j];
            j--;
        }
        r->addr[j + 1] = a;
    }
}

/* Test hook: send to a unicast address instead of the multicast group.
 * secure_getenv() ignores it in setuid/setcap programs. */
static int test_destination(struct sockaddr_in *dst)
{
    const char *t = secure_getenv("NSS_LLMNR_TEST_DEST");
    if (!t || !*t)
        return 0;
    return inet_pton(AF_INET, t, &dst->sin_addr) == 1;
}

static int llmnr_lookup(const char *label, struct result *r)
{
    struct iface ifs[MAX_IFACES];
    int nif, fd, one = 1, ttl = 255, got_valid = 0, qlen;
    uint8_t q[12 + 1 + 63 + 1 + 4], buf[1500];
    uint16_t id;
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(LLMNR_PORT) };
    int test = test_destination(&dst);

    r->n = 0;
    nif = get_ifaces(ifs);
    if (nif == 0 && !test)
        return -1;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one);
    one = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof one);

    if (getrandom(&id, sizeof id, GRND_NONBLOCK) != sizeof id)
        id = (uint16_t)(now_ms() ^ getpid());
    qlen = build_query(q, id, label);
    if (!test)
        inet_pton(AF_INET, LLMNR_GROUP, &dst.sin_addr);

    for (int attempt = 0; attempt < MAX_TRIES && !got_valid; attempt++) {
        long deadline;

        if (test) {
            sendto(fd, q, qlen, 0, (struct sockaddr *)&dst, sizeof dst);
        } else {
            for (int i = 0; i < nif; i++) {
                struct ip_mreqn mr = { .imr_address = ifs[i].addr };
                setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mr, sizeof mr);
                sendto(fd, q, qlen, 0, (struct sockaddr *)&dst, sizeof dst);
            }
        }

        deadline = now_ms() + FIRST_WAIT_MS;
        for (;;) {
            long left = deadline - now_ms();
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t n;

            if (left <= 0)
                break;
            if (poll(&pfd, 1, (int)left) <= 0)
                break;
            n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n <= 0)
                continue;
            if (ntohs(from.sin_port) != LLMNR_PORT)
                continue;
            if (!test && is_own_addr(from.sin_addr, ifs, nif))
                continue;
            if (parse_response(buf, (int)n, id, label, r)) {
                if (!got_valid) {
                    /* first answer: now collect everything else that arrives */
                    got_valid = 1;
                    deadline = now_ms() + COLLECT_MS;
                }
            }
        }
    }
    close(fd);

    if (r->n == 0)
        return got_valid ? 0 : -1;
    sort_result(r, ifs, nif);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  glibc NSS entry points                                            */
/* ------------------------------------------------------------------ */

#define ALIGN_PTR(p) (((uintptr_t)(p) + (sizeof(void *) - 1)) & ~(uintptr_t)(sizeof(void *) - 1))

enum nss_status _nss_llmnr_gethostbyname4_r(const char *name,
        struct gaih_addrtuple **pat, char *buffer, size_t buflen,
        int *errnop, int *herrnop, int32_t *ttlp)
{
    char label[64];
    struct result r;
    size_t namelen, need;
    char *p;
    struct gaih_addrtuple *prev = NULL;

    if (normalise_name(name, label) != 0) {
        *errnop = ENOENT;
        *herrnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }
    if (llmnr_lookup(label, &r) != 0 || r.n == 0) {
        *errnop = ENOENT;
        *herrnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }

    namelen = strlen(label) + 1;
    need = namelen + sizeof(void *) + r.n * sizeof(struct gaih_addrtuple);
    if (buflen < need) {
        *errnop = ERANGE;
        *herrnop = NETDB_INTERNAL;
        return NSS_STATUS_TRYAGAIN;
    }

    p = buffer;
    memcpy(p, label, namelen);
    char *nm = p;
    p = (char *)ALIGN_PTR(p + namelen);

    for (int i = 0; i < r.n; i++) {
        struct gaih_addrtuple *t = (struct gaih_addrtuple *)p;
        memset(t, 0, sizeof *t);
        t->name = (i == 0) ? nm : NULL;
        t->family = AF_INET;
        memcpy(t->addr, &r.addr[i].s_addr, 4);
        if (prev)
            prev->next = t;
        else
            *pat = t;
        prev = t;
        p += sizeof *t;
    }
    if (ttlp)
        *ttlp = 30;
    return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_llmnr_gethostbyname3_r(const char *name, int af,
        struct hostent *host, char *buffer, size_t buflen,
        int *errnop, int *herrnop, int32_t *ttlp, char **canonp)
{
    char label[64];
    struct result r;
    size_t namelen, need;
    char *p, *nm, **addrlist, **aliases;

    if (af == AF_UNSPEC)
        af = AF_INET;
    if (af != AF_INET || normalise_name(name, label) != 0 ||
        llmnr_lookup(label, &r) != 0 || r.n == 0) {
        *errnop = ENOENT;
        *herrnop = HOST_NOT_FOUND;
        return NSS_STATUS_NOTFOUND;
    }

    namelen = strlen(label) + 1;
    need = namelen + 2 * sizeof(void *) + r.n * 4 + (r.n + 2) * sizeof(char *);
    if (buflen < need) {
        *errnop = ERANGE;
        *herrnop = NETDB_INTERNAL;
        return NSS_STATUS_TRYAGAIN;
    }

    p = buffer;
    nm = p;
    memcpy(p, label, namelen);
    p = (char *)ALIGN_PTR(p + namelen);

    aliases = (char **)p;
    aliases[0] = NULL;
    p += sizeof(char *);

    addrlist = (char **)p;
    p += (r.n + 1) * sizeof(char *);
    for (int i = 0; i < r.n; i++) {
        memcpy(p, &r.addr[i].s_addr, 4);
        addrlist[i] = p;
        p += 4;
    }
    addrlist[r.n] = NULL;

    host->h_name = nm;
    host->h_aliases = aliases;
    host->h_addrtype = AF_INET;
    host->h_length = 4;
    host->h_addr_list = addrlist;
    if (ttlp)
        *ttlp = 30;
    if (canonp)
        *canonp = nm;
    return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_llmnr_gethostbyname2_r(const char *name, int af,
        struct hostent *host, char *buffer, size_t buflen,
        int *errnop, int *herrnop)
{
    return _nss_llmnr_gethostbyname3_r(name, af, host, buffer, buflen,
                                          errnop, herrnop, NULL, NULL);
}

enum nss_status _nss_llmnr_gethostbyname_r(const char *name,
        struct hostent *host, char *buffer, size_t buflen,
        int *errnop, int *herrnop)
{
    return _nss_llmnr_gethostbyname3_r(name, AF_INET, host, buffer, buflen,
                                          errnop, herrnop, NULL, NULL);
}
