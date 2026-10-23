package net.rete.relay

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/** Start / stop the relay, and the one setting it needs to survive Doze. */
class MainActivity : Activity() {
    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        status = TextView(this)
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad * 3, pad, pad)
            addView(TextView(this@MainActivity).apply {
                text = "oevm relay (rete#439 spike)\nBluetooth node ⇄ UDP over Tailscale ⇄ porta"
            })
            addView(button("Start relay") {
                requestPermissions(PERMS, 1)
                startForegroundService(Intent(this@MainActivity, RelayService::class.java))
            })
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

    override fun onResume() {
        super.onResume()
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        status.text = "\nBattery unrestricted: " + pm.isIgnoringBatteryOptimizations(packageName) +
            "\nLink state is in the notification."
    }

    private fun button(label: String, on: () -> Unit) =
        Button(this).apply { text = label; setOnClickListener { on() } }

    companion object {
        val PERMS = arrayOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.POST_NOTIFICATIONS,
        )
    }
}
