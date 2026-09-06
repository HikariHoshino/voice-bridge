package com.voicebridge.app.network

import com.voicebridge.app.model.DeliveryCode
import com.voicebridge.app.model.DeliveryResult
import com.voicebridge.app.storage.DeviceIdentity
import com.voicebridge.app.storage.TrustStore
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.SocketException
import java.util.LinkedHashMap
import java.util.concurrent.Executors
import java.util.concurrent.Semaphore
import java.util.concurrent.atomic.AtomicBoolean
import javax.net.ssl.SSLServerSocket
import javax.net.ssl.SSLSocket

class PeerServer(
    private val identity: DeviceIdentity,
    private val trustStore: TrustStore,
    private val callback: Callback,
) {
    interface Callback {
        fun approvePair(peerId: String, peerName: String, fingerprint: String, code: String): Boolean
        fun inject(text: String): DeliveryResult
        fun isReceiving(): Boolean
        fun onAuthenticatedActivity(peerId: String)
    }

    private val tls = TlsSockets(identity)
    private val workers = Executors.newFixedThreadPool(4)
    private val connectionSlots = Semaphore(8)
    private val pairingInProgress = AtomicBoolean(false)
    private val replayLock = Any()
    private val seenMessageIds = LinkedHashMap<String, Unit>(256, 0.75f, true)
    @Volatile private var running = false
    private var serverSocket: SSLServerSocket? = null
    private var acceptThread: Thread? = null

    val port: Int get() = serverSocket?.localPort ?: 0

    fun start() {
        if (running) return
        val socket = tls.serverFactory().createServerSocket(0) as SSLServerSocket
        socket.needClientAuth = false
        socket.enabledProtocols = socket.supportedProtocols.filter { it == "TLSv1.3" || it == "TLSv1.2" }.toTypedArray()
        serverSocket = socket
        running = true
        acceptThread = Thread({ acceptLoop(socket) }, "voicebridge-tls-server").apply {
            isDaemon = true
            start()
        }
    }

    fun stop() {
        running = false
        runCatching { serverSocket?.close() }
        serverSocket = null
        workers.shutdownNow()
    }

    private fun acceptLoop(socket: SSLServerSocket) {
        while (running) {
            try {
                val client = socket.accept() as SSLSocket
                if (!connectionSlots.tryAcquire()) {
                    client.close()
                    continue
                }
                workers.execute {
                    try {
                        handle(client)
                    } finally {
                        connectionSlots.release()
                    }
                }
            } catch (_: SocketException) {
                if (running) callback.onAuthenticatedActivity("")
            } catch (_: Exception) {
                // A malformed or exhausted connection must not kill discovery/receiving.
            }
        }
    }

    private fun handle(socket: SSLSocket) {
        socket.use {
            it.soTimeout = Protocol.SOCKET_TIMEOUT_MS
            it.startHandshake()
            val reader = BufferedReader(InputStreamReader(it.inputStream, Charsets.UTF_8))
            val writer = OutputStreamWriter(it.outputStream, Charsets.UTF_8)
            val line = reader.readLine() ?: return
            if (line.toByteArray(Charsets.UTF_8).size > Protocol.MAX_TEXT_BYTES + 8_192) return
            val request = runCatching { JSONObject(line) }.getOrNull() ?: return
            val response = process(request)
            writer.write(response.toString())
            writer.write("\n")
            writer.flush()
        }
    }

    internal fun process(request: JSONObject): JSONObject {
        if (request.optInt("version") != Protocol.VERSION) {
            return result(DeliveryCode.INVALID, "协议版本不兼容")
        }
        val peerId = request.optString("peerId")
        val certificateFingerprint = Protocol.verifiedClientFingerprint(request)
        if (peerId.isBlank() || certificateFingerprint == null) {
            return result(DeliveryCode.UNAUTHORIZED, "客户端证书签名无效")
        }
        return when (request.optString("type")) {
            "PAIR_REQUEST" -> pair(request, peerId, certificateFingerprint)
            "PING" -> authenticated(request, peerId, certificateFingerprint) {
                Protocol.message("PONG", "ok" to true)
            }
            "TEXT_PUSH" -> authenticated(request, peerId, certificateFingerprint) {
                receiveText(request)
            }
            else -> result(DeliveryCode.INVALID, "未知请求")
        }
    }

    private fun pair(request: JSONObject, peerId: String, fingerprint: String): JSONObject {
        if (!pairingInProgress.compareAndSet(false, true)) {
            return result(DeliveryCode.UNAUTHORIZED, "已有配对请求正在确认")
        }
        try {
        val peerName = request.optString("peerName").take(40)
        val nonce = request.optString("nonce")
        val secret = request.optString("secret")
        if (peerName.isBlank() || nonce.length !in 16..128 || secret.length !in 32..128) {
            return result(DeliveryCode.INVALID, "配对参数无效")
        }
        val code = Protocol.pairingCode(identity.fingerprint, fingerprint, nonce)
        if (!callback.approvePair(peerId, peerName, fingerprint, code)) {
            return result(DeliveryCode.UNAUTHORIZED, "目标设备拒绝配对")
        }
        trustStore.put(peerId, fingerprint, secret)
        callback.onAuthenticatedActivity(peerId)
        return Protocol.message("PAIR_RESULT", "ok" to true, "code" to code)
        } finally {
            pairingInProgress.set(false)
        }
    }

    private inline fun authenticated(
        request: JSONObject,
        peerId: String,
        fingerprint: String,
        action: () -> JSONObject,
    ): JSONObject {
        val trusted = trustStore.get(peerId)
            ?: return result(DeliveryCode.UNAUTHORIZED, "设备尚未配对")
        if (!Protocol.constantTimeEquals(trusted.fingerprint, fingerprint)) {
            return result(DeliveryCode.UNAUTHORIZED, "设备证书已变化，请重新配对")
        }
        val messageId = request.optString("messageId")
        val text = request.optString("text")
        val suppliedAuth = request.optString("auth")
        if (messageId.length !in 16..80 ||
            !Protocol.constantTimeEquals(suppliedAuth, Protocol.auth(trusted.secret, messageId, text))) {
            return result(DeliveryCode.UNAUTHORIZED, "消息认证失败")
        }
        synchronized(replayLock) {
            val replayKey = "$peerId:$messageId"
            if (seenMessageIds.containsKey(replayKey)) return result(DeliveryCode.INVALID, "重复消息已拒绝")
            seenMessageIds[replayKey] = Unit
            while (seenMessageIds.size > 256) seenMessageIds.remove(seenMessageIds.entries.first().key)
        }
        callback.onAuthenticatedActivity(peerId)
        return action()
    }

    private fun receiveText(request: JSONObject): JSONObject {
        if (!callback.isReceiving()) return result(DeliveryCode.PAUSED, "目标设备已暂停接收")
        val text = request.optString("text")
        if (text.isEmpty() || text.toByteArray(Charsets.UTF_8).size > Protocol.MAX_TEXT_BYTES) {
            return result(DeliveryCode.INVALID, "内容为空或超过 100 KiB")
        }
        val delivery = callback.inject(text)
        return result(delivery.code, delivery.detail)
    }

    private fun result(code: DeliveryCode, detail: String) =
        Protocol.message("TEXT_RESULT", "ok" to (code == DeliveryCode.INJECTED), "code" to code.wireName, "detail" to detail)
}
