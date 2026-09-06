package com.voicebridge.app.network

import com.voicebridge.app.model.DeliveryCode
import com.voicebridge.app.model.DeliveryResult
import com.voicebridge.app.model.Peer
import com.voicebridge.app.storage.DeviceIdentity
import com.voicebridge.app.storage.TrustStore
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.security.SecureRandom
import java.util.UUID
import javax.net.ssl.SSLSocket

class PeerClient(
    private val identity: DeviceIdentity,
    private val trustStore: TrustStore,
) {
    private val tls = TlsSockets(identity)

    fun pair(peer: Peer, localApproval: (code: String) -> Boolean): DeliveryResult {
        val nonceBytes = ByteArray(24).also { SecureRandom().nextBytes(it) }
        val nonce = java.util.Base64.getEncoder().withoutPadding().encodeToString(nonceBytes)
        val code = Protocol.pairingCode(identity.fingerprint, peer.fingerprint, nonce)
        if (!localApproval(code)) return DeliveryResult(DeliveryCode.UNAUTHORIZED, "已取消配对")
        val secret = TrustStore.newSecret()
        val response = transact(peer, signed(Protocol.message(
            "PAIR_REQUEST",
            "peerId" to identity.id,
            "peerName" to identity.name,
            "deviceType" to identity.type.wireName,
            "nonce" to nonce,
            "secret" to secret,
        )))
        if (response.optBoolean("ok") && response.optString("code") == code) {
            trustStore.put(peer.id, peer.fingerprint, secret)
            return DeliveryResult(DeliveryCode.INJECTED, "配对成功")
        }
        return parseResult(response)
    }

    fun ping(peer: Peer): DeliveryResult = authenticated(peer, "PING", "")

    fun sendText(peer: Peer, text: String): DeliveryResult {
        if (text.isEmpty() || text.toByteArray(Charsets.UTF_8).size > Protocol.MAX_TEXT_BYTES) {
            return DeliveryResult(DeliveryCode.INVALID, "内容为空或超过 100 KiB")
        }
        return authenticated(peer, "TEXT_PUSH", text)
    }

    private fun authenticated(peer: Peer, type: String, text: String): DeliveryResult {
        val trusted = trustStore.get(peer.id)
            ?: return DeliveryResult(DeliveryCode.UNAUTHORIZED, "设备尚未配对")
        if (!Protocol.constantTimeEquals(trusted.fingerprint, peer.fingerprint)) {
            return DeliveryResult(DeliveryCode.UNAUTHORIZED, "设备证书已变化，请重新配对")
        }
        val messageId = UUID.randomUUID().toString()
        val response = runCatching {
            transact(peer, signed(Protocol.message(
                type,
                "peerId" to identity.id,
                "messageId" to messageId,
                "text" to text,
                "auth" to Protocol.auth(trusted.secret, messageId, text),
            )))
        }.getOrElse {
            return DeliveryResult(DeliveryCode.UNREACHABLE, "无法连接目标设备")
        }
        if (type == "PING" && response.optString("type") == "PONG" && response.optBoolean("ok")) {
            return DeliveryResult(DeliveryCode.INJECTED)
        }
        return parseResult(response)
    }

    private fun transact(peer: Peer, request: JSONObject): JSONObject {
        val socket = tls.clientFactory().createSocket(peer.host, peer.port) as SSLSocket
        socket.use {
            it.soTimeout = Protocol.SOCKET_TIMEOUT_MS
            it.enabledProtocols = it.supportedProtocols.filter { protocol -> protocol == "TLSv1.3" || protocol == "TLSv1.2" }.toTypedArray()
            it.startHandshake()
            val actualFingerprint = TlsSockets.serverFingerprint(it.session)
            require(Protocol.constantTimeEquals(actualFingerprint, peer.fingerprint)) { "证书指纹不匹配" }
            val writer = OutputStreamWriter(it.outputStream, Charsets.UTF_8)
            writer.write(request.toString())
            writer.write("\n")
            writer.flush()
            val response = BufferedReader(InputStreamReader(it.inputStream, Charsets.UTF_8)).readLine()
                ?: error("目标设备未返回结果")
            require(response.toByteArray(Charsets.UTF_8).size <= 16_384) { "返回结果过长" }
            return JSONObject(response)
        }
    }

    private fun parseResult(response: JSONObject): DeliveryResult = DeliveryResult(
        DeliveryCode.fromWire(response.optString("code")),
        response.optString("detail"),
    )

    private fun signed(message: JSONObject): JSONObject {
        message.put("fingerprint", identity.fingerprint)
        message.put("certificate", identity.encodedCertificate)
        message.put("signature", identity.sign(Protocol.canonicalPayload(message)))
        return message
    }
}
