package com.voicebridge.app.network

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import com.voicebridge.app.model.ConnectionState
import com.voicebridge.app.model.DeviceType
import com.voicebridge.app.model.Peer
import com.voicebridge.app.storage.DeviceIdentity
import java.util.concurrent.ConcurrentHashMap

class NsdDiscovery(
    private val context: Context,
    private val identity: DeviceIdentity,
    private val port: Int,
    private val onPeerFound: (Peer) -> Unit,
    private val onPeerLost: (String) -> Unit,
) {
    private val nsd = context.getSystemService(NsdManager::class.java)
    private val connectivity = context.getSystemService(ConnectivityManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private val serviceToPeer = ConcurrentHashMap<String, String>()
    private var registration: NsdManager.RegistrationListener? = null
    private var discovery: NsdManager.DiscoveryListener? = null
    private var networkCallback: ConnectivityManager.NetworkCallback? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var udpDiscovery: UdpDiscovery? = null
    private val refreshRunnable = Runnable {
        if (started) {
            unregisterNsd()
            runCatching { registerAndDiscover() }
        }
    }
    @Volatile private var started = false

    fun start() = handler.post {
        if (started) return@post
        started = true
        multicastLock = context.getSystemService(WifiManager::class.java)
            .createMulticastLock("voicebridge-mdns")
            .apply { setReferenceCounted(false); acquire() }
        registerNetworkCallback()
        udpDiscovery = UdpDiscovery(identity, port, onPeerFound).also { it.start() }
        runCatching { registerAndDiscover() }
    }

    fun stop() = handler.post {
        started = false
        unregisterNsd()
        udpDiscovery?.stop()
        udpDiscovery = null
        networkCallback?.let { runCatching { connectivity.unregisterNetworkCallback(it) } }
        networkCallback = null
        multicastLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
    }

    private fun registerNetworkCallback() {
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) { refresh() }
            override fun onLost(network: Network) { refresh() }
            override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) { refresh() }
        }
        networkCallback = callback
        connectivity.registerDefaultNetworkCallback(callback)
    }

    private fun refresh() {
        handler.removeCallbacks(refreshRunnable)
        handler.postDelayed(refreshRunnable, 400)
    }

    private fun registerAndDiscover() {
        val serviceInfo = NsdServiceInfo().apply {
            serviceName = "VoiceBridge-${identity.id.take(8)}"
            serviceType = Protocol.SERVICE_TYPE
            port = this@NsdDiscovery.port
            setAttribute("id", identity.id)
            setAttribute("name", identity.name)
            setAttribute("type", identity.type.wireName)
            setAttribute("fp", identity.fingerprint)
            setAttribute("v", Protocol.VERSION.toString())
        }
        val registrationListener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(serviceInfo: NsdServiceInfo) = Unit
            override fun onRegistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) = Unit
            override fun onServiceUnregistered(serviceInfo: NsdServiceInfo) = Unit
            override fun onUnregistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) = Unit
        }
        registration = registrationListener
        nsd.registerService(serviceInfo, NsdManager.PROTOCOL_DNS_SD, registrationListener)

        val discoveryListener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) = Unit
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) = Unit
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) = Unit
            override fun onDiscoveryStopped(serviceType: String) = Unit
            override fun onServiceFound(info: NsdServiceInfo) {
                if (!info.serviceName.startsWith("VoiceBridge-")) return
                nsd.resolveService(info, object : NsdManager.ResolveListener {
                    override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) = Unit
                    override fun onServiceResolved(resolved: NsdServiceInfo) {
                        val id = resolved.attribute("id") ?: return
                        if (id == identity.id) return
                        val fingerprint = resolved.attribute("fp") ?: return
                        val name = resolved.attribute("name") ?: "Android 设备"
                        val type = DeviceType.fromWire(resolved.attribute("type") ?: "phone")
                        val host = resolved.host?.hostAddress ?: return
                        serviceToPeer[resolved.serviceName] = id
                        onPeerFound(Peer(
                            id = id,
                            name = name,
                            type = type,
                            host = host.substringBefore('%'),
                            port = resolved.port,
                            fingerprint = fingerprint,
                            trusted = false,
                            state = ConnectionState.DISCOVERED,
                            lastSeenMillis = System.currentTimeMillis(),
                        ))
                    }
                })
            }

            override fun onServiceLost(info: NsdServiceInfo) {
                serviceToPeer.remove(info.serviceName)?.let(onPeerLost)
            }
        }
        discovery = discoveryListener
        nsd.discoverServices(Protocol.SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener)
    }

    private fun unregisterNsd() {
        handler.removeCallbacks(refreshRunnable)
        discovery?.let { runCatching { nsd.stopServiceDiscovery(it) } }
        registration?.let { runCatching { nsd.unregisterService(it) } }
        discovery = null
        registration = null
    }

    private fun NsdServiceInfo.attribute(key: String): String? =
        attributes[key]?.toString(Charsets.UTF_8)?.takeIf { it.isNotBlank() }
}
