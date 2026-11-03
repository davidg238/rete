package net.rete.relay

import java.io.File
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class FrameTest {
    // The golden vector protocol/RELAY-BLE.md names, shared with the node's
    // nodus/oevm/tests/test_relay_frame.c. Unit tests run from the app module's directory.
    private val vector: ByteArray = File("../../../protocol/fixtures/relay/frame.hex").readText().trim()
        .chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    private val golden = Frame(16970, 6969, "fd7a:115c:a1e0::1", "hi".toByteArray())

    @Test fun encodeMatchesTheVector() = assertArrayEquals(vector, golden.encode())

    @Test fun decodeTheVector() = assertEquals(golden, Frame.decode(vector))

    @Test fun roundTrips() {
        val f = Frame(49152, 65535, "100.87.217.33", ByteArray(2048) { it.toByte() })
        assertEquals(f, Frame.decode(f.encode()))
    }

    @Test fun decodeHonoursLength() {
        val bytes = Frame(1, 2, "h", byteArrayOf(9, 9)).encode() + byteArrayOf(7, 7, 7)
        assertEquals(Frame(1, 2, "h", byteArrayOf(9, 9)), Frame.decode(bytes, bytes.size - 3))
    }

    @Test fun malformedIsNull() {
        assertNull(Frame.decode(byteArrayOf(0, 1, 0)))                    // short
        assertNull(Frame.decode(byteArrayOf(0, 1, 0, 2, 5, 65)))          // host runs past the end
    }
}
