package com.voicebridge.app.accessibility

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class TextInsertionTest {
    @Test
    fun `markdown indentation line breaks and uncommon unicode remain exact`() {
        val incoming = """新的问题出现了：
1. 急需新袜子架，原有太小。
   2. 课桌扩展空间（侧边袋）
      3. new手提袋（转移）
         4. 🌟🌟🌟文言文教辅！
            5. 买一堆书
               6. 买一堆笔记本（原）
                  7. 错题本

```kotlin
val rare = \"𠮷野家\tمرحبا\"
```"""
        assertEquals("前缀\n$incoming\n后缀", TextInsertion.splice("前缀\n\n后缀", 3, 3, incoming))
    }

    @Test
    fun `selection is replaced without normalizing crlf spaces or tabs`() {
        val incoming = "  first\r\n\r\n\tsecond  "
        assertEquals("A$incoming\r\nD", TextInsertion.splice("AB C\r\nD", 1, 4, incoming))
    }

    @Test
    fun `utf16 cursor after emoji is preserved`() {
        val original = "甲😀乙"
        val cursorAfterEmoji = "甲😀".length
        assertEquals("甲😀\n  * 丙乙", TextInsertion.splice(original, cursorAfterEmoji, cursorAfterEmoji, "\n  * 丙"))
    }

    @Test
    fun `invalid selections fail instead of overwriting a control`() {
        assertThrows(IllegalArgumentException::class.java) { TextInsertion.splice("abc", -1, 0, "x") }
        assertThrows(IllegalArgumentException::class.java) { TextInsertion.splice("abc", 2, 4, "x") }
    }
}
