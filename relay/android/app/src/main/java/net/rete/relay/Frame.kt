package net.rete.relay

/**
 * One datagram on the Bluetooth L2CAP channel — the wire of
 * nodus/oevm/src/backend/zephyr/udp_ble.c (rete#439). One L2CAP SDU is one frame,
 * both directions, integers big-endian:
 *
 *   u8 version (1) | u16 localPort | u16 remotePort | u8 hostLen | host (text) | payload
 *
 * localPort is the node's bound port. remotePort/host are the destination going up
 * (node -> relay) and the sender coming down (relay -> node). The host is TEXT, the
 * same string the node's seam carries, so the relay never re-formats an address the
 * node will compare.
 */
data class Frame(val localPort: Int, val remotePort: Int, val host: String, val payload: ByteArray) {

    fun encode(): ByteArray {
        val h = host.toByteArray(Charsets.US_ASCII)
        require(h.size <= 255) { "host longer than 255 bytes" }
        val out = ByteArray(HEADER + h.size + payload.size)
        out[0] = VERSION.toByte()
        out[1] = (localPort ushr 8).toByte(); out[2] = localPort.toByte()
        out[3] = (remotePort ushr 8).toByte(); out[4] = remotePort.toByte()
        out[5] = h.size.toByte()
        h.copyInto(out, HEADER)
        payload.copyInto(out, HEADER + h.size)
        return out
    }

    override fun equals(other: Any?) = other is Frame && localPort == other.localPort &&
        remotePort == other.remotePort && host == other.host && payload.contentEquals(other.payload)
    override fun hashCode() = (localPort * 31 + remotePort) * 31 + host.hashCode()

    companion object {
        const val VERSION = 1
        const val HEADER = 6

        /** The frame in buf[0, len), or null when it is malformed (dropped, as UDP would). */
        fun decode(buf: ByteArray, len: Int = buf.size): Frame? {
            if (len < HEADER || buf[0].toInt() != VERSION) return null
            val hl = buf[5].toInt() and 0xff
            if (len < HEADER + hl) return null
            val lport = ((buf[1].toInt() and 0xff) shl 8) or (buf[2].toInt() and 0xff)
            val rport = ((buf[3].toInt() and 0xff) shl 8) or (buf[4].toInt() and 0xff)
            val host = String(buf, HEADER, hl, Charsets.US_ASCII)
            return Frame(lport, rport, host, buf.copyOfRange(HEADER + hl, len))
        }
    }
}
