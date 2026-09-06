package com.voicebridge.app.storage

import android.content.Context
import org.json.JSONObject
import java.security.SecureRandom

data class TrustedPeer(val id: String, val fingerprint: String, val secret: String)

class TrustStore(context: Context) {
    private val prefs = context.getSharedPreferences("trusted_peers", Context.MODE_PRIVATE)

    fun get(peerId: String): TrustedPeer? {
        val raw = prefs.getString(peerId, null) ?: return null
        return runCatching {
            val json = JSONObject(raw)
            TrustedPeer(peerId, json.getString("fingerprint"), json.getString("secret"))
        }.getOrNull()
    }

    fun put(peerId: String, fingerprint: String, secret: String) {
        val value = JSONObject()
            .put("fingerprint", fingerprint)
            .put("secret", secret)
            .toString()
        prefs.edit().putString(peerId, value).apply()
    }

    fun remove(peerId: String) = prefs.edit().remove(peerId).apply()

    companion object {
        fun newSecret(): String {
            val bytes = ByteArray(32).also { SecureRandom().nextBytes(it) }
            return java.util.Base64.getEncoder().withoutPadding().encodeToString(bytes)
        }
    }
}
