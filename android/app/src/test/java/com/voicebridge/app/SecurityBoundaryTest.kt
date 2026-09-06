package com.voicebridge.app

import org.junit.Assert.assertFalse
import org.junit.Test
import java.io.File

class SecurityBoundaryTest {
    @Test
    fun `android app declares no input method service`() {
        val manifest = File("src/main/AndroidManifest.xml").readText(Charsets.UTF_8)
        assertFalse(manifest.contains("BIND_INPUT_METHOD"))
        assertFalse(manifest.contains("InputMethodService"))
    }

    @Test
    fun `android implementation never uses clipboard api`() {
        val source = File("src/main/java").walkTopDown()
            .filter { it.isFile && it.extension in setOf("kt", "java") }
            .joinToString("\n") { it.readText(Charsets.UTF_8) }
        assertFalse(source.contains("ClipboardManager"))
        assertFalse(source.contains("ClipData"))
        assertFalse(source.contains("BIND_INPUT_METHOD"))
        assertFalse(source.contains("InputMethodService"))
    }
}
