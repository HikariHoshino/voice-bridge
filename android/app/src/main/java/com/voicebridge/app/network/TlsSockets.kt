package com.voicebridge.app.network

import com.voicebridge.app.storage.DeviceIdentity
import java.security.KeyStore
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.cert.X509Certificate
import javax.net.ssl.KeyManagerFactory
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSession
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

class TlsSockets(identity: DeviceIdentity) {
    private val context: SSLContext

    init {
        val keyManagers = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm()).run {
            init(identity.keyStore(), null)
            keyManagers
        }
        val trustOnlyForPinnedApplicationPeers = arrayOf<TrustManager>(object : X509TrustManager {
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
            override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) = Unit
            override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) = Unit
        })
        context = SSLContext.getInstance("TLS").apply {
            init(keyManagers, trustOnlyForPinnedApplicationPeers, SecureRandom())
        }
    }

    fun serverFactory() = context.serverSocketFactory
    fun clientFactory() = context.socketFactory

    companion object {
        fun serverFingerprint(session: SSLSession): String {
            val certificate = session.peerCertificates.first() as X509Certificate
            return MessageDigest.getInstance("SHA-256")
                .digest(certificate.encoded)
                .joinToString("") { "%02x".format(it) }
        }
    }
}
