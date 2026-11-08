/* oevm/src/backend/zephyr/udp_ble.c — SPIKE, rete#439.
   The datagram seam (udp_backend.h) over a Bluetooth LE L2CAP connection-oriented
   channel to a relay — David's phone — which forwards each datagram as real UDP over
   Tailscale to porta. Linked instead of udp_zephyr.c when the build has CONFIG_BT:
   the backend follows the radio. nsl, the kernel and porta see the same seam.

   THE WIRE is protocol/RELAY-BLE.md: one L2CAP SDU = one datagram, the local port,
   the remote port and the remote host as TEXT before the payload; the codec is
   relay_frame.h. The relay is a dumb forwarder: it keeps one UDP socket per (node, local_port), so
   porta sees a distinct source port per node socket and answers it as usual.

   PAIR ONCE (David, 2026-10-23): the channel needs an encrypted link (BT_SECURITY_L2,
   LE Secure Connections, Just Works), so the phone must bond before it can talk. The
   bond is kept in settings; once one exists every new pairing is refused, and a phone
   without the keys can connect but never open the channel. A full flash erase drops
   the bond, which is how a lost phone is replaced.

   One phone, one channel: a second connection is refused. While no channel is up,
   a send answers -1 and is dropped — UDP's own semantics, and porta already retries.
   A refused send sets errno as the socket backend's sendto would: ENOTCONN (no
   channel) or EAGAIN (no buffer), so the console tee can tell dead from busy.

   TWO CALLERS: the VM thread and the console tee's thread (log_udp.c) both use the
   seam, so the socket table and the transmit path are under sock_lock.

   rete#440, THE DOORBELL: recv_cb rings a VM socket's channel after the frame is in its
   ring (readable before the bell). It runs once per frame on the Bluetooth thread, so
   nothing here can spin, and the constant token coalesces a burst into one pending
   wakeup: no reader thread and no parking, and udp_receive's empty answer needs no
   restore. */
#include "udp_backend.h"
#include "relay_frame.h"
#include "../../pulse_backend.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>

#define RELAY_PSM       0x0080          /* fixed for the spike; the relay dials it */
#define SDU_MTU         (RELAY_FRAME_HDR_MAX + 2048)    /* udp.c reads into a 2048 B buffer */
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
static int sock_port[MAX_SOCKS];      /* 0 = free; under sock_lock */
static int sock_chan[MAX_SOCKS];      /* rete#440: the doorbell's channel + 1; 0 = none (OTA, the tee) */
static K_MUTEX_DEFINE(sock_lock);

/* Received datagrams, whole frames, each stored as [u16 len][frame], ONE RING PER SOCKET,
   as UDP keeps one queue per socket. Written by the BT RX thread, read by whichever thread
   polls that socket. rete#449: it was one ring for all sockets, and a socket took a
   datagram only when the head was its own, so the OTA fetch thread and the VM's porta
   socket each waited behind the other's reply: the fetch timed out at block 105 while the
   check-in counted misses. 1 KB a socket (the same 4 KB) holds a whole 512 B batch or three
   OTA blocks, and both are stop-and-wait. */
#define RING_PER_SOCK   (RING_BYTES / MAX_SOCKS)
static struct { uint8_t b[RING_PER_SOCK]; size_t head, tail, used; } rings[MAX_SOCKS];
static K_MUTEX_DEFINE(ring_lock);

static void ring_put(int s, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        rings[s].b[rings[s].tail] = p[i]; rings[s].tail = (rings[s].tail + 1) % RING_PER_SOCK;
    }
    rings[s].used += n;
}
static void ring_peek(int s, size_t off, uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = rings[s].b[(rings[s].head + off + i) % RING_PER_SOCK];
}
static void ring_drop(int s, size_t n) {
    rings[s].head = (rings[s].head + n) % RING_PER_SOCK; rings[s].used -= n;
}

static struct net_buf *alloc_buf_cb(struct bt_l2cap_chan *chan) {
    (void)chan;
    return net_buf_alloc(&rx_pool, K_NO_WAIT);
}

static int recv_cb(struct bt_l2cap_chan *chan, struct net_buf *buf) {
    (void)chan;
    uint16_t len = buf->len;
    k_mutex_lock(&ring_lock, K_FOREVER);
    struct relay_frame f;
    /* malformed, for an unbound port, or a full ring: drop, as UDP would. sock_port is
       read without sock_lock (udp_receive takes sock_lock then ring_lock; this takes
       only ring_lock): a socket bound or closed mid-frame costs that one datagram. */
    int s = -1;
    if (relay_frame_parse(buf->data, len, &f) == 0)
        for (int i = 0; i < MAX_SOCKS; i++) if (sock_port[i] == f.local_port) { s = i; break; }
    int bell = 0;
    if (s >= 0 && rings[s].used + 2 + len <= RING_PER_SOCK) {
        uint8_t l[2]; sys_put_be16(len, l);
        ring_put(s, l, 2);
        ring_put(s, buf->data, len);
        bell = sock_chan[s];
    }
    k_mutex_unlock(&ring_lock);
    if (bell) pulse_post_isr_value(bell - 1, TAG_SMI(s));   /* after ring_put: readable first */
    return 0;                                             /* the stack frees buf */
}

static void connected_cb(struct bt_l2cap_chan *chan) {
    printk("ble: relay channel up (tx mtu %u, rx mtu %u)\n",
           BT_L2CAP_LE_CHAN(chan)->tx.mtu, BT_L2CAP_LE_CHAN(chan)->rx.mtu);
    chan_up = true;
}
/* The channel is the only reason the link exists: when it closes, drop the link too,
   so the node advertises again. Left up, a link without a channel holds the node's one
   connection and the relay, scanning for an advertiser, never finds it (ACC01, 2026-10-23). */
static void disconnected_cb(struct bt_l2cap_chan *chan) {
    chan_up = false;
    printk("ble: relay channel down\n");
    if (chan->conn) (void)bt_conn_disconnect(chan->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
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
    .sec_level = BT_SECURITY_L2,                          /* encrypted: bonded phones only */
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
static void conn_connected(struct bt_conn *conn, uint8_t err) {
    (void)conn;
    printk("ble: link up (err %u)\n", err);
}
static void conn_disconnected(struct bt_conn *conn, uint8_t reason) {
    (void)conn;
    printk("ble: link down (reason 0x%02x)\n", reason);
}
BT_CONN_CB_DEFINE(conn_cbs) = {
    .connected = conn_connected,
    .disconnected = conn_disconnected,
    .recycled = recycled_cb,
};

static void count_bond(const struct bt_bond_info *info, void *n) {
    (void)info;
    (*(int *)n)++;
}

/* Pair once: the first phone bonds, every later pairing is refused. */
static enum bt_security_err pairing_accept(struct bt_conn *conn,
                                           const struct bt_conn_pairing_feat *feat) {
    (void)conn; (void)feat;
    int bonds = 0;
    bt_foreach_bond(BT_ID_DEFAULT, count_bond, &bonds);
    if (bonds > 0) {
        printk("ble: pairing refused, already bonded\n");
        return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
    }
    return BT_SECURITY_ERR_SUCCESS;
}

static void pairing_complete(struct bt_conn *conn, bool bonded) {
    (void)conn;
    printk("ble: paired, %s\n", bonded ? "bonded" : "NOT bonded");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
    (void)conn;
    printk("ble: pairing failed (%d)\n", reason);
}

static struct bt_conn_auth_cb auth_cb = { .pairing_accept = pairing_accept };
static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

static void open_locked(void) {
    if (inited) return;
    inited = true;
    int err = bt_enable(NULL);
    if (err) { printk("ble: bt_enable failed (%d)\n", err); return; }
    (void)settings_subsys_init();                         /* idempotent; platform_zephyr.c calls it too */
    (void)settings_load_subtree("bt");                    /* the bond, if there is one */
    bt_conn_auth_cb_register(&auth_cb);
    bt_conn_auth_info_cb_register(&auth_info_cb);
    err = bt_l2cap_server_register(&server);
    if (err) { printk("ble: l2cap server failed (%d)\n", err); return; }
    adv_start();
    printk("ble: advertising as \"%s\", relay psm 0x%04x\n", CONFIG_BT_DEVICE_NAME, RELAY_PSM);
}

void udp_open(void) {
    k_mutex_lock(&sock_lock, K_FOREVER);
    open_locked();
    k_mutex_unlock(&sock_lock);
}

int udp_socket_bind(int port) {
    int fd = -1;
    k_mutex_lock(&sock_lock, K_FOREVER);
    open_locked();
    for (int i = 0; i < MAX_SOCKS; i++) {
        if (sock_port[i] == 0) {
            sock_port[i] = port ? port : EPHEMERAL_BASE + i;
            fd = i;
            break;
        }
    }
    k_mutex_unlock(&sock_lock);
    return fd;
}

static int send_locked(int sock, const char *host, int port, const uint8_t *data, int len) {
    if (sock < 0 || sock >= MAX_SOCKS || sock_port[sock] == 0) { errno = EBADF; return -1; }
    if (!chan_up) { errno = ENOTCONN; return -1; }
    size_t hl = strlen(host);
    if (len < 0 || RELAY_FRAME_HDR + hl + (size_t)len > SDU_MTU) { errno = EMSGSIZE; return -1; }
    struct net_buf *buf = net_buf_alloc(&tx_pool, K_NO_WAIT);
    if (buf == NULL) { errno = EAGAIN; return -1; }       /* both in flight: drop */
    net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
    int h = relay_frame_put_header(net_buf_tail(buf), net_buf_tailroom(buf),
                                   (uint16_t)sock_port[sock], (uint16_t)port, host, hl);
    if (h < 0) { net_buf_unref(buf); errno = EMSGSIZE; return -1; }
    net_buf_add(buf, (size_t)h);
    net_buf_add_mem(buf, data, (size_t)len);
    int err = bt_l2cap_chan_send(&le_chan.chan, buf);
    if (err < 0) { net_buf_unref(buf); errno = -err; return -1; }
    return len;
}

int udp_send(int sock, const char *host, int port, const uint8_t *data, int len) {
    k_mutex_lock(&sock_lock, K_FOREVER);
    int n = send_locked(sock, host, port, data, len);
    int e = errno;
    k_mutex_unlock(&sock_lock);
    errno = e;
    return n;
}

/* Pops this socket's oldest datagram. */
int udp_receive(int sock, uint8_t *buf, int buf_cap,
                char *host_out, int host_cap, int *sender_port) {
    int n = -1;
    k_mutex_lock(&sock_lock, K_FOREVER);                  /* then ring_lock; recv_cb takes only ring_lock */
    if (sock < 0 || sock >= MAX_SOCKS || sock_port[sock] == 0) { k_mutex_unlock(&sock_lock); return -1; }
    k_mutex_lock(&ring_lock, K_FOREVER);
    while (rings[sock].used > 0) {
        uint8_t l[2], h[RELAY_FRAME_HDR_MAX];
        struct relay_frame f;
        ring_peek(sock, 0, l, 2);
        uint16_t flen = sys_get_be16(l);
        ring_peek(sock, 2, h, flen < sizeof h ? flen : sizeof h);
        if (relay_frame_parse(h, flen, &f) != 0 || f.local_port != sock_port[sock]) {
            ring_drop(sock, 2 + flen);                    /* a previous binding's, or malformed */
            continue;
        }
        if (host_cap > 0) {
            size_t c = f.host_len < (size_t)host_cap - 1 ? f.host_len : (size_t)host_cap - 1;
            memcpy(host_out, h + f.host_off, c);
            host_out[c] = '\0';
        }
        *sender_port = f.remote_port;
        size_t c = f.payload_len < (size_t)buf_cap ? f.payload_len : (size_t)buf_cap;
        ring_peek(sock, 2 + f.payload_off, buf, c);
        ring_drop(sock, 2 + flen);
        n = (int)c;
        break;
    }
    k_mutex_unlock(&ring_lock);
    k_mutex_unlock(&sock_lock);
    return n;
}

int udp_doorbell(int sock, int chan_id) {
    if (sock < 0 || sock >= MAX_SOCKS) return -1;         /* every socket this backend opens has a slot */
    k_mutex_lock(&ring_lock, K_FOREVER);                  /* recv_cb reads it under ring_lock */
    sock_chan[sock] = chan_id + 1;
    k_mutex_unlock(&ring_lock);
    return 0;
}

void udp_close(int sock) {
    k_mutex_lock(&sock_lock, K_FOREVER);
    if (sock >= 0 && sock < MAX_SOCKS) {
        sock_port[sock] = 0;
        k_mutex_lock(&ring_lock, K_FOREVER);          /* the next binding starts empty */
        rings[sock].head = rings[sock].tail = rings[sock].used = 0;
        sock_chan[sock] = 0;
        k_mutex_unlock(&ring_lock);
    }
    k_mutex_unlock(&sock_lock);
}
