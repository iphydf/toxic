// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.content.Context

/** toxic's command-line options, which it only reads when it starts. */
data class Startup(
    /** Off by default: TCP relays only, like toxic --force-tcp. */
    val udp: Boolean = false,
    val lanDiscovery: Boolean = true,
    val proxy: String = NO_PROXY,
    val proxyHost: String = "",
    val proxyPort: Int = 0,
) {
    val proxied get() = proxy != NO_PROXY

    fun save(context: Context) {
        preferences(context).edit()
            .putBoolean("udp", udp)
            .putBoolean("lan_discovery", lanDiscovery)
            .putString("proxy", proxy)
            .putString("proxy_host", proxyHost)
            .putInt("proxy_port", proxyPort)
            .commit()
    }

    /** A proxy turns UDP off too, or UDP would bypass it. */
    fun args() = buildList {
        if (!udp || proxied) add("--force-tcp")
        if (!lanDiscovery) add("--no-lan")

        // toxic exits on a proxy without a host and port.
        if (proxied && proxyHost.isNotEmpty() && validPort(proxyPort)) {
            add(if (proxy == SOCKS5) "--SOCKS5-proxy" else "--HTTP-proxy")
            add(proxyHost)
            add(proxyPort.toString())
        }
    }.toTypedArray()

    companion object {
        const val NO_PROXY = "none"
        const val SOCKS5 = "socks5"
        const val HTTP = "http"

        private fun preferences(context: Context) = context.getSharedPreferences("startup", Context.MODE_PRIVATE)

        fun load(context: Context): Startup {
            val saved = preferences(context)
            val default = Startup()
            return Startup(
                udp = saved.getBoolean("udp", default.udp),
                lanDiscovery = saved.getBoolean("lan_discovery", default.lanDiscovery),
                proxy = saved.getString("proxy", null) ?: default.proxy,
                proxyHost = saved.getString("proxy_host", null) ?: default.proxyHost,
                proxyPort = saved.getInt("proxy_port", default.proxyPort),
            )
        }

        fun validPort(port: Int) = port in 1..65535
    }
}
