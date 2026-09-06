package com.voicebridge.app.network

import com.voicebridge.app.model.ConnectionState
import com.voicebridge.app.model.DeviceType
import com.voicebridge.app.model.Peer
import com.voicebridge.app.storage.DeviceIdentity
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.MulticastSocket
import java.net.SocketTimeoutException

class UdpDiscovery(
    private val identity: DeviceIdentity,
    private val servicePort: Int,
    private val onPeerFound: (Peer) -> Unit,
) {
    @Volatile private var running = false
    private var socket: MulticastSocket? = null

    fun start() {
        if (running) return
        running = true
        Thread({
            val group = InetAddress.getByName(GROUP)
            val multicast = MulticastSocket(null).apply {
                reuseAddress = true
                bind(InetSocketAddress(PORT))
                soTimeout = 1_000
                timeToLive = 1
                joinGroup(group)
            }
            if (!running) {
                multicast.close()
                return@Thread
            }
            socket = multicast
            Thread({ announceLoop(multicast, group) }, "voicebridge-udp-announce").apply { isDaemon = true; start() }
            receiveLoop(multicast)
        }, "voicebridge-udp-receive").apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        socket?.close()
        socket = null
    }

    private fun announceLoop(multicast: MulticastSocket, group: InetAddress) {
        while (running) {
            val body = JSONObject()
                .put("app", "voicebridge")
                .put("version", Protocol.VERSION)
                .put("id", identity.id)
                .put("name", identity.name)
                .put("deviceType", identity.type.wireName)
                .put("fingerprint", identity.fingerprint)
                .put("port", servicePort)
                .toString().toByteArray(Charsets.UTF_8)
            if (body.size <= MAX_BEACON_BYTES) {
                runCatching { multicast.send(DatagramPacket(body, body.size, group, PORT)) }
            }
            repeat(5) {
                if (!running) return
                Thread.sleep(1_000)
            }
        }
    }

    private fun receiveLoop(multicast: MulticastSocket) {
        val buffer = ByteArray(MAX_BEACON_BYTES + 1)
        while (running) {
            val packet = DatagramPacket(buffer, buffer.size)
            try {
                multicast.receive(packet)
            } catch (_: SocketTimeoutException) {
                continue
            } catch (_: Exception) {
                if (running) continue else return
            }
            if (packet.length > MAX_BEACON_BYTES) continue
            val json = runCatching {
                JSONObject(String(packet.data, packet.offset, packet.length, Charsets.UTF_8))
            }.getOrNull() ?: continue
            val id = json.optString("id")
            if (json.optString("app") != "voicebridge" || json.optInt("version") != Protocol.VERSION || id.isBlank() || id == identity.id) continue
            val port = json.optInt("port")
            val fingerprint = json.optString("fingerprint")
            if (port !in 1..65535 || fingerprint.length != 64) continue
            onPeerFound(Peer(
                id = id,
                name = json.optString("name", "Android 设备").take(40),
                type = DeviceType.fromWire(json.optString("deviceType")),
                host = packet.address.hostAddress?.substringBefore('%') ?: continue,
                port = port,
                fingerprint = fingerprint,
                trusted = false,
                state = ConnectionState.DISCOVERED,
                lastSeenMillis = System.currentTimeMillis(),
            ))
        }
    }

    companion object {
        private const val GROUP = "224.0.0.167"
        private const val PORT = 53318
        private const val MAX_BEACON_BYTES = 2_048
    }
}
