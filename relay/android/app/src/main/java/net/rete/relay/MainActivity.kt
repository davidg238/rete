package net.rete.relay

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/** Start / stop the relay, and the one setting it needs to survive Doze. */
class MainActivity : Activity() {
    private lateinit var status: TextView
    private val tick = Handler(Looper.getMainLooper())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        status = TextView(this)
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            // targetSdk 35 is edge-to-edge: keep the content clear of the system bars.
            setOnApplyWindowInsetsListener { v, insets ->
                val bars = insets.getInsets(android.view.WindowInsets.Type.systemBars())
                v.setPadding(pad, bars.top + pad, pad, bars.bottom + pad)
                insets
            }
            addView(TextView(this@MainActivity).apply {
                text = "oevm relay (rete#439 spike)\nBluetooth node ⇄ UDP over Tailscale ⇄ porta"
            })
            addView(button("Start relay") { startRelay() })
            addView(button("Stop relay") {
                startService(Intent(this@MainActivity, RelayService::class.java).setAction(RelayService.ACTION_STOP))
            })
            addView(button("Allow unrestricted battery") {
                startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                    Uri.parse("package:$packageName")))
            })
            addView(status)
        }
        setContentView(col)
    }

    /** A connectedDevice service may only start once BLUETOOTH_CONNECT is granted
        (Android 14+), so the start waits for the grant instead of racing it. */
    private fun startRelay() {
        val missing = PERMS.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) { requestPermissions(missing.toTypedArray(), REQ); return }
        try {
            startForegroundService(Intent(this, RelayService::class.java))
        } catch (e: Exception) {
            RelayService.lastState = "could not start: $e"
        }
    }

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, results: IntArray) {
        if (code != REQ) return
        val denied = perms.filterIndexed { i, _ -> results.getOrNull(i) != PackageManager.PERMISSION_GRANTED }
        if (denied.isEmpty()) startRelay()
        else RelayService.lastState = "permission denied: " + denied.joinToString { it.substringAfterLast('.') }
    }

    override fun onResume() { super.onResume(); refresh() }
    override fun onPause() { super.onPause(); tick.removeCallbacksAndMessages(null) }

    private fun refresh() {
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        status.text = "\nRelay: " + RelayService.lastState +
            "\nup ${RelayService.upCount} · down ${RelayService.downCount} datagrams" +
            "\nBattery unrestricted: " + pm.isIgnoringBatteryOptimizations(packageName)
        tick.postDelayed({ refresh() }, 1000)
    }

    private fun button(label: String, on: () -> Unit) =
        Button(this).apply { text = label; setOnClickListener { on() } }

    companion object {
        const val REQ = 1
        val PERMS = arrayOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.POST_NOTIFICATIONS,
        )
    }
}
