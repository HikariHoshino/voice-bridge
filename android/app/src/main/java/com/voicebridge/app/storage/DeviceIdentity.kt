package com.voicebridge.app.storage

import android.content.Context
import android.content.res.Configuration
import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import com.voicebridge.app.model.DeviceType
import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.MessageDigest
import java.security.Signature
import java.security.cert.X509Certificate
import java.util.Date
import java.util.UUID
import javax.security.auth.x500.X500Principal

class DeviceIdentity(private val context: Context) {
    private val prefs = context.getSharedPreferences("identity", Context.MODE_PRIVATE)
    private val alias = "voicebridge_tls_identity_v2"

    val id: String by lazy {
        prefs.getString("node_id", null) ?: UUID.randomUUID().toString().also {
            prefs.edit().putString("node_id", it).apply()
        }
    }

    val name: String
        get() = prefs.getString("device_name", null)
            ?: "${Build.MANUFACTURER} ${Build.MODEL}".trim().ifBlank { "Android 设备" }

    val type: DeviceType
        get() = if (context.resources.configuration.smallestScreenWidthDp >= 600) {
            DeviceType.TABLET
        } else {
            DeviceType.PHONE
        }

    fun setName(value: String) {
        val safe = value.trim().take(40)
        if (safe.isNotEmpty()) prefs.edit().putString("device_name", safe).apply()
    }

    fun keyStore(): KeyStore {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        if (!store.containsAlias(alias)) {
            val now = System.currentTimeMillis()
            val spec = KeyGenParameterSpec.Builder(
                alias,
                KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY,
            )
                .setAlgorithmParameterSpec(java.security.spec.ECGenParameterSpec("secp256r1"))
                .setDigests(KeyProperties.DIGEST_SHA256)
                .setCertificateSubject(X500Principal("CN=VoiceBridge-$id"))
                .setCertificateSerialNumber(BigInteger(128, java.security.SecureRandom()))
                .setCertificateNotBefore(Date(now - 86_400_000L))
                .setCertificateNotAfter(Date(now + 3_153_600_000_000L))
                .build()
            KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore")
                .apply { initialize(spec) }
                .generateKeyPair()
        }
        return store
    }

    val certificate: X509Certificate
        get() = keyStore().getCertificate(alias) as X509Certificate

    val fingerprint: String
        get() = MessageDigest.getInstance("SHA-256")
            .digest(certificate.encoded)
            .joinToString("") { "%02x".format(it) }

    val encodedCertificate: String
        get() = java.util.Base64.getEncoder().encodeToString(certificate.encoded)

    fun sign(payload: ByteArray): String {
        val privateKey = keyStore().getKey(alias, null)
        val signature = Signature.getInstance("SHA256withECDSA").apply {
            initSign(privateKey as java.security.PrivateKey)
            update(payload)
        }.sign()
        return java.util.Base64.getEncoder().withoutPadding().encodeToString(signature)
    }
}
