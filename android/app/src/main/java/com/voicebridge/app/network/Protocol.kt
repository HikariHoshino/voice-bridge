package com.voicebridge.app.network

import org.json.JSONObject
import java.security.MessageDigest
import java.security.Signature
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

object Protocol {
    const val VERSION = 2
    const val SERVICE_TYPE = "_voicebridge._tcp."
    const val MAX_TEXT_BYTES = 100 * 1024
    const val SOCKET_TIMEOUT_MS = 8_000

    fun message(type: String, vararg values: Pair<String, Any?>): JSONObject =
        JSONObject().put("type", type).put("version", VERSION).apply {
            values.forEach { (key, value) -> put(key, value) }
        }

    fun pairingCode(firstFingerprint: String, secondFingerprint: String, nonce: String): String {
        val material = listOf(firstFingerprint, secondFingerprint).sorted().joinToString(":") + ":$nonce"
        val digest = MessageDigest.getInstance("SHA-256").digest(material.toByteArray(Charsets.UTF_8))
        val number = ((digest[0].toInt() and 0xff) shl 16) or
            ((digest[1].toInt() and 0xff) shl 8) or (digest[2].toInt() and 0xff)
        return (number % 1_000_000).toString().padStart(6, '0')
    }

    fun auth(secret: String, messageId: String, text: String): String {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(java.util.Base64.getDecoder().decode(secret), "HmacSHA256"))
        return java.util.Base64.getEncoder().withoutPadding().encodeToString(
            mac.doFinal((messageId + "\u0000" + text).toByteArray(Charsets.UTF_8)),
        )
    }

    fun constantTimeEquals(first: String, second: String): Boolean =
        MessageDigest.isEqual(first.toByteArray(Charsets.UTF_8), second.toByteArray(Charsets.UTF_8))

    fun canonicalPayload(message: JSONObject): ByteArray {
        val output = java.io.ByteArrayOutputStream()
        message.keys().asSequence().filter { it != "signature" }.sorted().forEach { key ->
            val keyBytes = key.toByteArray(Charsets.UTF_8)
            val valueBytes = message.get(key).toString().toByteArray(Charsets.UTF_8)
            output.write(keyBytes.size.toString().toByteArray(Charsets.US_ASCII))
            output.write(':'.code)
            output.write(keyBytes)
            output.write(valueBytes.size.toString().toByteArray(Charsets.US_ASCII))
            output.write(':'.code)
            output.write(valueBytes)
        }
        return output.toByteArray()
    }

    fun verifiedClientFingerprint(message: JSONObject): String? = runCatching {
        val certificateBytes = java.util.Base64.getDecoder().decode(message.getString("certificate"))
        val certificate = CertificateFactory.getInstance("X.509")
            .generateCertificate(certificateBytes.inputStream()) as X509Certificate
        certificate.checkValidity()
        val fingerprint = MessageDigest.getInstance("SHA-256")
            .digest(certificate.encoded)
            .joinToString("") { "%02x".format(it) }
        if (!constantTimeEquals(fingerprint, message.getString("fingerprint"))) return null
        val verifier = Signature.getInstance("SHA256withECDSA").apply {
            initVerify(certificate.publicKey)
            update(canonicalPayload(message))
        }
        if (!verifier.verify(java.util.Base64.getDecoder().decode(message.getString("signature")))) return null
        fingerprint
    }.getOrNull()
}
