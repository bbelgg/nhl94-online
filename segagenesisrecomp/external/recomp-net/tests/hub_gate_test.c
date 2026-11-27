/* The host hub relays every guest datagram to every other peer, and peers
 * attribute a packet to the seat its body names. This checks the hub's gate
 * (rnet_transport.c hub_packet_from / hub_learn_and_forward): a datagram that
 * names a seat other than its real source, a host-only packet from a guest,
 * a bad checksum, or a stranger claiming a joined seat must be dropped and
 * never relayed. Honest traffic must still be delivered and relayed. */
#include "transport/rnet_transport.h"
#include "protocol/rnet_protocol.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
int main(void)
{
    printf("hub_gate_test: skipped on Windows (POSIX sockets in the test)\n");
    return 0;
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAGIC 0x4E48u
#define SID 0x1234u

static int g_failures;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

static int udp_bound(struct sockaddr_in *out)
{
    socklen_t len = sizeof(*out);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || bind(fd, (struct sockaddr *)out, sizeof(*out)) != 0 ||
        getsockname(fd, (struct sockaddr *)out, &len) != 0)
        return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

static void send_to(int fd, const struct sockaddr_in *to, const rnet_u8 *buf, int n)
{
    (void)sendto(fd, buf, (size_t)n, 0, (const struct sockaddr *)to, sizeof(*to));
}

/* Datagrams are on loopback; give them a moment before polling. */
static void settle(void) { usleep(20000); }

/* Everything the hub delivers to its own session right now (0 = nothing). */
static int hub_drain(RNetTransport *t)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int got = 0, n;
    settle();
    while ((n = rnet_transport_recv(t, buf, sizeof(buf))) > 0)
        got++;
    return got;
}

static int guest_drain(int fd)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int got = 0;
    settle();
    while (recv(fd, buf, sizeof(buf), 0) > 0)
        got++;
    return got;
}

static int ready_from(rnet_u8 *buf, size_t cap, rnet_u8 slot)
{
    return rnet_proto_encode_ready(buf, cap, MAGIC, SID, slot);
}

int main(void)
{
    RNetTransport hub;
    struct sockaddr_in hub_addr, a1, a2, ax;
    socklen_t len = sizeof(hub_addr);
    rnet_u8 pkt[RNET_MAX_PACKET];
    int g1, g2, gx, n;

    rnet_transport_init(&hub);
    if (rnet_transport_start_lan_hub(&hub, "127.0.0.1:0") != 0 ||
        getsockname((int)hub.sock, (struct sockaddr *)&hub_addr, &len) != 0)
    {
        fprintf(stderr, "FAIL: could not start a loopback hub\n");
        return 1;
    }
    g1 = udp_bound(&a1);
    g2 = udp_bound(&a2);
    gx = udp_bound(&ax);
    if (g1 < 0 || g2 < 0 || gx < 0)
    {
        fprintf(stderr, "FAIL: could not open guest sockets\n");
        return 1;
    }

    /* Honest seats join: each is delivered and pinned. */
    n = ready_from(pkt, sizeof(pkt), 1);
    send_to(g1, &hub_addr, pkt, n);
    expect(hub_drain(&hub) == 1, "seat 1 READY is delivered");
    n = ready_from(pkt, sizeof(pkt), 2);
    send_to(g2, &hub_addr, pkt, n);
    expect(hub_drain(&hub) == 1, "seat 2 READY is delivered");
    expect(guest_drain(g1) == 1, "seat 2 READY is relayed to seat 1");
    (void)guest_drain(g2);

    /* Seat 2 speaking as seat 1, or as the host. */
    n = ready_from(pkt, sizeof(pkt), 1);
    send_to(g2, &hub_addr, pkt, n);
    n = ready_from(pkt, sizeof(pkt), 0);
    send_to(g2, &hub_addr, pkt, n);
    expect(hub_drain(&hub) == 0, "a seat naming another seat is dropped");
    expect(guest_drain(g1) == 0, "an impersonating packet is not relayed");

    /* Host-only packets from a guest. */
    n = rnet_proto_encode_start(pkt, sizeof(pkt), MAGIC, SID, 0);
    send_to(g1, &hub_addr, pkt, n);
    n = rnet_proto_encode_delay_sync(pkt, sizeof(pkt), MAGIC, SID, 200, 0);
    send_to(g1, &hub_addr, pkt, n);
    expect(hub_drain(&hub) == 0, "START / DELAY_SYNC from a guest are dropped");
    expect(guest_drain(g2) == 0, "host-only packets from a guest are not relayed");

    /* A broken checksum. */
    n = ready_from(pkt, sizeof(pkt), 1);
    pkt[n - 1] ^= 0xFFu;
    send_to(g1, &hub_addr, pkt, n);
    expect(hub_drain(&hub) == 0, "a bad checksum is dropped");

    /* A stranger claiming a joined seat cannot re-point it. */
    n = ready_from(pkt, sizeof(pkt), 1);
    send_to(gx, &hub_addr, pkt, n);
    expect(hub_drain(&hub) == 0, "a stranger claiming a joined seat is dropped");
    expect(guest_drain(g2) == 0, "the stranger's packet is not relayed");
    n = ready_from(pkt, sizeof(pkt), 2);
    send_to(g1, &hub_addr, pkt, n);
    (void)hub_drain(&hub);
    n = ready_from(pkt, sizeof(pkt), 1);
    send_to(g1, &hub_addr, pkt, n);
    expect(hub_drain(&hub) == 1, "seat 1 is still reachable at its own address");
    expect(guest_drain(gx) == 0, "the stranger receives no relayed traffic");
    expect(guest_drain(g2) == 1, "seat 2 still receives seat 1's traffic");

    rnet_transport_shutdown(&hub);
    close(g1);
    close(g2);
    close(gx);
    if (g_failures)
    {
        fprintf(stderr, "hub_gate_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("hub_gate_test: ok\n");
    return 0;
}
#endif
