/* oevm/src/backend/zephyr/udp_ble.c — SPIKE, rete#439.
   The datagram seam (udp_backend.h) over a Bluetooth LE L2CAP connection-oriented
   channel to a relay — David's phone — which forwards each datagram as real UDP over
   Tailscale to porta. Linked instead of udp_zephyr.c when the build has CONFIG_BT:
   the backend follows the radio. nsl, the kernel and porta see the same seam.

   THE WIRE (one L2CAP SDU = one datagram, both directions, all integers big-endian):
       u8  version            1
       u16 local_port         the node's bound port (its "socket")
       u16 remote_port        node->relay: destination; relay->node: sender
       u8  host_len           then host_len bytes of the remote host as TEXT,
                              exactly the string the seam carries (IPv6 or IPv4)
       ... payload
   The relay is a dumb forwarder: it keeps one UDP socket per (node, local_port), so
   porta sees a distinct source port per node socket and answers it as usual.

   One phone, one channel: a second connection is refused. While no channel is up,
   a send answers -1 and is dropped — UDP's own semantics, and porta already retries. */
#include "udp_backend.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/uuid.h>

#define RELAY_VERSION   1
#define RELAY_PSM       0x0080          /* fixed for the spike; the relay dials it */
#define HDR_MAX         (1 + 2 + 2 + 1 + 255)
#define SDU_MTU         (HDR_MAX + 2048)    /* udp.c reads into a 2048 B buffer */
#define MAX_SOCKS       4
#define EPHEMERAL_BASE  49152
#define RING_BYTES      4096

/* The relay's service UUID, so the phone can filter its scan: 6f657673-6d00-4000-8000-00000000439a */
#define RELAY_UUID BT_UUID_128_ENCODE(0x6f657673, 0x6d00, 0x4000, 0x8000, 0x00000000439aULL)

NET_BUF_POOL_DEFINE(tx_pool, 2, BT_L2CAP_SDU_BUF_SIZE(SDU_MTU), CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);
NET_BUF_POOL_DEFINE(rx_pool, 1, BT_L2CAP_SDU_BUF_SIZE(SDU_MTU), 8, NULL);

static struct bt_l2cap_le_chan le_chan;
static volatile bool chan_up;
static bool inited;
static int sock_port[MAX_SOCKS];      /* 0 = free */

/* Received datagrams, whole frames, each stored as [u16 len][frame]. Written by the BT
   RX thread, read by the VM thread polling udp_receive. */
static uint8_t ring[RING_BYTES];
static size_t r_head, r_tail, r_used;
static K_MUTEX_DEFINE(ring_lock);

static void ring_put(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { ring[r_tail] = p[i]; r_tail = (r_tail + 1) % RING_BYTES; }
    r_used += n;
}
static void ring_peek(size_t off, uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = ring[(r_head + off + i) % RING_BYTES];
}
static void ring_drop(size_t n) { r_head = (r_head + n) % RING_BYTES; r_used -= n; }

static struct net_buf *alloc_buf_cb(struct bt_l2cap_chan *chan) {
    (void)chan;
    return net_buf_alloc(&rx_pool, K_NO_WAIT);
}

static int recv_cb(struct bt_l2cap_chan *chan, struct net_buf *buf) {
    (void)chan;
    uint16_t len = buf->len;
    k_mutex_lock(&ring_lock, K_FOREVER);
    if (len >= 6 && r_used + 2 + len <= RING_BYTES) {     /* full ring: drop, as UDP would */
        uint8_t l[2]; sys_put_be16(len, l);
        ring_put(l, 2);
        ring_put(buf->data, len);
    }
    k_mutex_unlock(&ring_lock);
    return 0;                                             /* the stack frees buf */
}

static void connected_cb(struct bt_l2cap_chan *chan) {
    printk("ble: relay channel up (tx mtu %u, rx mtu %u)\n",
           BT_L2CAP_LE_CHAN(chan)->tx.mtu, BT_L2CAP_LE_CHAN(chan)->rx.mtu);
    chan_up = true;
}
static void disconnected_cb(struct bt_l2cap_chan *chan) {
    (void)chan;
    chan_up = false;
    printk("ble: relay channel down\n");
}

static const struct bt_l2cap_chan_ops chan_ops = {
    .connected = connected_cb,
    .disconnected = disconnected_cb,
    .alloc_buf = alloc_buf_cb,
    .recv = recv_cb,
};

static int accept_cb(struct bt_conn *conn, struct bt_l2cap_server *server,
                     struct bt_l2cap_chan **chan) {
    (void)conn; (void)server;
    if (chan_up) return -EBUSY;                           /* one relay at a time */
    memset(&le_chan, 0, sizeof le_chan);
    le_chan.chan.ops = &chan_ops;
    le_chan.rx.mtu = SDU_MTU;
    *chan = &le_chan.chan;
    return 0;
}

static struct bt_l2cap_server server = {
    .psm = RELAY_PSM,
    .sec_level = BT_SECURITY_L1,                          /* spike: no pairing yet */
    .accept = accept_cb,
};

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, RELAY_UUID),
};
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void adv_start(void) {
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err && err != -EALREADY) printk("ble: advertising failed (%d)\n", err);
}

/* A connection consumes the advertiser; start it again once the connection object is
   free, so the relay can always find the node after a drop. */
static void recycled_cb(void) { adv_start(); }
BT_CONN_CB_DEFINE(conn_cbs) = { .recycled = recycled_cb };

void udp_open(void) {
    if (inited) return;
    inited = true;
    int err = bt_enable(NULL);
    if (err) { printk("ble: bt_enable failed (%d)\n", err); return; }
    err = bt_l2cap_server_register(&server);
    if (err) { printk("ble: l2cap server failed (%d)\n", err); return; }
    adv_start();
    printk("ble: advertising as \"%s\", relay psm 0x%04x\n", CONFIG_BT_DEVICE_NAME, RELAY_PSM);
}

int udp_socket_bind(int port) {
    udp_open();
    for (int i = 0; i < MAX_SOCKS; i++) {
        if (sock_port[i] == 0) {
            sock_port[i] = port ? port : EPHEMERAL_BASE + i;
            return i;
        }
    }
    return -1;
}

int udp_send(int sock, const char *host, int port, const uint8_t *data, int len) {
    if (sock < 0 || sock >= MAX_SOCKS || sock_port[sock] == 0 || !chan_up) return -1;
    size_t hl = strlen(host);
    if (hl > 255 || len < 0 || 6 + hl + (size_t)len > SDU_MTU) return -1;
    struct net_buf *buf = net_buf_alloc(&tx_pool, K_NO_WAIT);
    if (buf == NULL) return -1;                           /* both in flight: drop */
    net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
    net_buf_add_u8(buf, RELAY_VERSION);
    net_buf_add_be16(buf, (uint16_t)sock_port[sock]);
    net_buf_add_be16(buf, (uint16_t)port);
    net_buf_add_u8(buf, (uint8_t)hl);
    net_buf_add_mem(buf, host, hl);
    net_buf_add_mem(buf, data, (size_t)len);
    int err = bt_l2cap_chan_send(&le_chan.chan, buf);
    if (err < 0) { net_buf_unref(buf); return -1; }
    return len;
}

static bool bound(int port) {
    for (int i = 0; i < MAX_SOCKS; i++) if (sock_port[i] == port) return true;
    return false;
}

/* Pops the oldest datagram if it is for this socket. One for an unbound port is
   dropped; one for another bound socket waits for that socket's poll. */
int udp_receive(int sock, uint8_t *buf, int buf_cap,
                char *host_out, int host_cap, int *sender_port) {
    if (sock < 0 || sock >= MAX_SOCKS || sock_port[sock] == 0) return -1;
    int n = -1;
    k_mutex_lock(&ring_lock, K_FOREVER);
    while (r_used > 0) {
        uint8_t h[8];
        ring_peek(0, h, 8);
        uint16_t flen = sys_get_be16(h);
        uint16_t lport = sys_get_be16(h + 3), rport = sys_get_be16(h + 5);
        uint8_t hl = h[7];
        size_t plen = (flen >= 6u + hl) ? flen - 6u - hl : 0;
        if (h[2] != RELAY_VERSION || flen < 6u + hl || !bound(lport)) {
            ring_drop(2 + flen);                          /* malformed or unbound */
            continue;
        }
        if (lport != sock_port[sock]) break;              /* another socket's */
        if (host_cap > 0) {
            size_t c = hl < (size_t)host_cap - 1 ? hl : (size_t)host_cap - 1;
            ring_peek(8, (uint8_t *)host_out, c);
            host_out[c] = '\0';
        }
        *sender_port = rport;
        size_t c = plen < (size_t)buf_cap ? plen : (size_t)buf_cap;
        ring_peek(8 + hl, buf, c);
        ring_drop(2 + flen);
        n = (int)c;
        break;
    }
    k_mutex_unlock(&ring_lock);
    return n;
}

void udp_close(int sock) {
    if (sock >= 0 && sock < MAX_SOCKS) sock_port[sock] = 0;
}
