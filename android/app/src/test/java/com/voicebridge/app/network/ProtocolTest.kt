package com.voicebridge.app.network

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Base64

class ProtocolTest {
    @Test
    fun `pairing code is symmetric and always six digits`() {
        val first = Protocol.pairingCode("aa", "bb", "fixed-nonce-123456")
        val second = Protocol.pairingCode("bb", "aa", "fixed-nonce-123456")
        assertEquals(first, second)
        assertEquals("550877", first)
        assertTrue(first.matches(Regex("\\d{6}")))
    }

    @Test
    fun `message authentication binds id and exact text`() {
        val secret = Base64.getEncoder().withoutPadding().encodeToString(ByteArray(32) { it.toByte() })
        val formatted = "1. first\n   2. second\r\n\t终"
        val signature = Protocol.auth(secret, "message-123456789", formatted)
        assertEquals("sAQ/ITqSgZxinwC6QtkjccQ7l55xHH/3LMRJOOsGOmc", signature)
        assertEquals(signature, Protocol.auth(secret, "message-123456789", formatted))
        assertNotEquals(signature, Protocol.auth(secret, "message-123456780", formatted))
        assertNotEquals(signature, Protocol.auth(secret, "message-123456789", formatted.replace("\n", " ")))
    }

    @Test
    fun `constant time comparison accepts only exact value`() {
        assertTrue(Protocol.constantTimeEquals("same", "same"))
        assertFalse(Protocol.constantTimeEquals("same", "Same"))
    }
}
