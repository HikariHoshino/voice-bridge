package com.voicebridge.app.accessibility

object TextInsertion {
    fun splice(original: String, selectionStart: Int, selectionEnd: Int, incoming: String): String {
        require(selectionStart in 0..original.length) { "selectionStart out of range" }
        require(selectionEnd in selectionStart..original.length) { "selectionEnd out of range" }
        return buildString(original.length - (selectionEnd - selectionStart) + incoming.length) {
            append(original, 0, selectionStart)
            append(incoming)
            append(original, selectionEnd, original.length)
        }
    }
}
