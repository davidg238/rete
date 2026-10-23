package net.rete.relay

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class FrameTest {
    @Test fun bytesMatchTheNodeWire() {
        // What udp_ble.c's udp_send builds for sock port 16970 -> [fd7a::1]:6979, payload "hi".
        val f = Frame(16970, 6979, "fd7a::1", "hi".toByteArray())
        val want = byteArrayOf(1, 0x42, 0x4a, 0x1b, 0x43, 7) + "fd7a::1".toByteArray() + "hi".toByteArray()
        assertArrayEquals(want, f.encode())
    }

    @Test fun roundTrips() {
        val f = Frame(49152, 65535, "100.87.217.33", ByteArray(2048) { it.toByte() })
        assertEquals(f, Frame.decode(f.encode()))
    }

    @Test fun decodeHonoursLength() {
        val bytes = Frame(1, 2, "h", byteArrayOf(9, 9)).encode() + byteArrayOf(7, 7, 7)
        assertEquals(Frame(1, 2, "h", byteArrayOf(9, 9)), Frame.decode(bytes, bytes.size - 3))
    }

    @Test fun malformedIsNull() {
        assertNull(Frame.decode(byteArrayOf(1, 0, 0)))                    // short
        assertNull(Frame.decode(byteArrayOf(2, 0, 1, 0, 2, 0)))           // wrong version
        assertNull(Frame.decode(byteArrayOf(1, 0, 1, 0, 2, 5, 65)))       // host runs past the end
    }
}
