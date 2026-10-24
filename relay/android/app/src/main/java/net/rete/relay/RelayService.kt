package net.rete.relay

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.os.ParcelUuid
import android.os.PowerManager
import android.util.Log
import java.io.IOException
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

/**
 * The porta relay (rete#439; the contract is protocol/RELAY-BLE.md). Finds the node by
 * its service UUID, bonds once, opens the encrypted L2CAP channel on PSM 0x80, and forwards: each frame up becomes a UDP datagram from a
 * socket kept per node port; each datagram back on that socket becomes a frame down.
 * Tailscale carries the UDP — nothing here knows about it.
 *
 * A connectedDevice foreground service: no time limit (unlike dataSync), and the
 * notification is the operator's view of the link. Doze survival is what the spike
 * measures; the app must be set to "unrestricted" battery for it.
 */
class RelayService : Service() {

    @Volatile private var running = false
    private var worker: Thread? = null
    private var wake: PowerManager.WakeLock? = null
    private val up = AtomicLong(); private val down = AtomicLong()
    @Volatile private var state = "starting"

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) { stopSelf(); return START_NOT_STICKY }
        try {
            startForeground(NOTIF_ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } catch (e: Exception) {
            lastState = "could not go foreground: $e"; log(lastState); stopSelf(); return START_NOT_STICKY
        }
        if (!running) {
            running = true
            // A partial wake lock keeps the CPU forwarding while the screen is off. Its
            // battery cost on the phone is one of the things the spike reports.
            wake = (getSystemService(POWER_SERVICE) as PowerManager)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "oevm-relay:link").apply { acquire() }
            worker = Thread({ loop() }, "relay-link").apply { start() }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        running = false
        worker?.interrupt()
        session?.close()
        wake?.let { if (it.isHeld) it.release() }
        super.onDestroy()
    }

    // ---- the link loop: find, connect, forward, and start over on any failure ----

    @Volatile private var session: Session? = null

    private fun loop() {
        while (running) {
            try {
                setState("scanning")
                val dev = scan() ?: continue
                if (!bond(dev)) continue
                setState("connecting ${dev.address}")
                val sock = dev.createL2capChannel(PSM)
                sock.connect()
                Session(sock).also { session = it }.run()
            } catch (e: SecurityException) {
                setState("missing Bluetooth permission"); log("permission: $e"); sleep(10_000)
            } catch (e: Exception) {
                fail("link: $e")
            } finally {
                session?.close(); session = null
            }
            if (running) { setState("link down; retrying"); sleep(2_000) }
        }
    }

    /** The node opens its channel to a bonded phone only (protocol/RELAY-BLE.md): bond
     *  first, once (LE Secure Connections, Just Works — Android asks David to confirm). */
    @Suppress("MissingPermission")
    private fun bond(dev: BluetoothDevice): Boolean {
        if (dev.bondState == BluetoothDevice.BOND_BONDED) return true
        setState("pairing ${dev.address}")
        if (dev.bondState == BluetoothDevice.BOND_NONE && !dev.createBond()) {
            fail("createBond refused"); return false
        }
        repeat(60) {
            if (dev.bondState == BluetoothDevice.BOND_BONDED) return true
            if (dev.bondState == BluetoothDevice.BOND_NONE && it > 0) { fail("pairing failed"); return false }
            sleep(500)
        }
        fail("pairing timed out"); return false
    }

    @Suppress("MissingPermission")
    private fun scan(): BluetoothDevice? {
        val scanner = (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter?.bluetoothLeScanner
            ?: run { setState("Bluetooth off"); sleep(5_000); return null }
        var found: BluetoothDevice? = null
        val latch = CountDownLatch(1)
        val cb = object : ScanCallback() {
            override fun onScanResult(type: Int, r: ScanResult) { found = r.device; latch.countDown() }
            override fun onScanFailed(code: Int) { log("scan failed $code"); latch.countDown() }
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner.startScan(listOf(filter), settings, cb)
        latch.await(15, TimeUnit.SECONDS)
        scanner.stopScan(cb)
        return found
    }

    /** One connected channel: a reader thread here, one UDP receiver thread per node port. */
    private inner class Session(private val bt: BluetoothSocket) {
        private val out: OutputStream = bt.outputStream
        private val udp = ConcurrentHashMap<Int, DatagramSocket>()
        // The host text the node used for each (address, port) it sent to, so a reply
        // goes back under the SAME string — the node may compare it to its literal.
        private val hostText = ConcurrentHashMap<Pair<InetAddress, Int>, String>()

        fun run() {
            setState("linked ${bt.remoteDevice.address}")
            val buf = ByteArray(maxOf(bt.maxReceivePacketSize, 4096))
            while (running) {
                val n = bt.inputStream.read(buf)          // L2CAP keeps SDU boundaries
                if (n < 0) throw IOException("channel closed")
                val f = Frame.decode(buf, n) ?: continue
                val addr = InetAddress.getByName(f.host)
                hostText[addr to f.remotePort] = f.host
                socketFor(f.localPort).send(DatagramPacket(f.payload, f.payload.size, addr, f.remotePort))
                bump(up)
            }
        }

        private fun socketFor(lport: Int): DatagramSocket = udp.getOrPut(lport) {
            DatagramSocket().also { s -> Thread({ receive(lport, s) }, "relay-udp-$lport").start() }
        }

        private fun receive(lport: Int, s: DatagramSocket) {
            val buf = ByteArray(2048)
            try {
                while (running && !s.isClosed) {
                    val p = DatagramPacket(buf, buf.size)
                    s.receive(p)
                    val host = hostText[p.address to p.port] ?: p.address.hostAddress!!.substringBefore('%')
                    val frame = Frame(lport, p.port, host, p.data.copyOfRange(0, p.length)).encode()
                    synchronized(out) { out.write(frame) }  // one write = one SDU
                    bump(down)
                }
            } catch (e: Exception) {
                if (!s.isClosed) { fail("udp $lport: $e"); close() }
            }
        }

        fun close() {
            udp.values.forEach { it.close() }
            try { bt.close() } catch (_: IOException) {}
        }
    }

    // ---- the notification is the status display ----

    private fun bump(c: AtomicLong) {
        val n = c.incrementAndGet()
        if (c === up) upCount = n else downCount = n
        if (n % 25 == 0L) refresh()
    }
    private fun setState(s: String) { state = s; lastState = s; log(s); refresh() }
    private fun refresh() =
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).notify(NOTIF_ID, notification())

    private fun notification(): Notification {
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Relay link", NotificationManager.IMPORTANCE_LOW))
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("oevm relay: $state")
            .setContentText("up ${up.get()} · down ${down.get()} datagrams")
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }

    private fun log(s: String) = Log.i(TAG, s)
    /** Kept apart from the state, which the retry loop overwrites within seconds: the
     *  status screen shows the last failure until the next one (no adb on this phone). */
    private fun fail(s: String) {
        lastError = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US).format(java.util.Date()) + " " + s
        log(s)
    }
    private fun sleep(ms: Long) = try { Thread.sleep(ms) } catch (_: InterruptedException) {}

    companion object {
        /** Read by MainActivity's status line (same process). */
        @Volatile var lastState = "not started"
        @Volatile var lastError = "none"
        @Volatile var upCount = 0L
        @Volatile var downCount = 0L
        const val TAG = "oevmrelay"
        const val ACTION_STOP = "net.rete.relay.STOP"
        const val PSM = 0x80
        const val NOTIF_ID = 439
        const val CHANNEL = "relay"
        /** udp_ble.c's RELAY_UUID. */
        val SERVICE_UUID: java.util.UUID = java.util.UUID.fromString("6f657673-6d00-4000-8000-00000000439a")
    }
}
