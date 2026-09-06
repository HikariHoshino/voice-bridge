package com.voicebridge.app.accessibility

import android.accessibilityservice.AccessibilityService
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import com.voicebridge.app.model.DeliveryCode
import com.voicebridge.app.model.DeliveryResult
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

class BridgeAccessibilityService : AccessibilityService() {
    override fun onServiceConnected() {
        instance = this
        availabilityListener?.invoke(true)
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) = Unit
    override fun onInterrupt() = Unit

    override fun onDestroy() {
        if (instance === this) instance = null
        availabilityListener?.invoke(false)
        super.onDestroy()
    }

    private fun injectOnMainThread(value: String): DeliveryResult {
        val focused = rootInActiveWindow?.findFocus(AccessibilityNodeInfo.FOCUS_INPUT)
            ?: return DeliveryResult(DeliveryCode.NO_FOCUS, "目标设备当前没有可编辑光标")
        try {
            if (!focused.isEditable || !focused.isFocused) {
                return DeliveryResult(DeliveryCode.NO_FOCUS, "目标设备当前没有可编辑光标")
            }
            if (focused.isPassword || isPasswordInputType(focused.inputType)) {
                return DeliveryResult(DeliveryCode.PASSWORD_FIELD, "为保护隐私，声桥拒绝向密码框注入")
            }
            if (focused.actionList.none { it.id == AccessibilityNodeInfo.ACTION_SET_TEXT }) {
                return DeliveryResult(DeliveryCode.UNSUPPORTED_EDITOR, "当前输入框不支持可靠的无障碍写入")
            }

            val original = focused.text?.toString() ?: ""
            val selectionStart = focused.textSelectionStart
            val selectionEnd = focused.textSelectionEnd
            if (selectionStart < 0 || selectionEnd < selectionStart || selectionEnd > original.length) {
                return DeliveryResult(DeliveryCode.UNSUPPORTED_EDITOR, "无法可靠读取当前光标位置")
            }
            val updated = TextInsertion.splice(original, selectionStart, selectionEnd, value)
            val setTextArgs = Bundle().apply {
                putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, updated)
            }
            if (!focused.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, setTextArgs)) {
                return DeliveryResult(DeliveryCode.UNSUPPORTED_EDITOR, "当前输入框拒绝了无障碍写入")
            }
            val newCursor = selectionStart + value.length
            val selectionArgs = Bundle().apply {
                putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, newCursor)
                putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, newCursor)
            }
            focused.performAction(AccessibilityNodeInfo.ACTION_SET_SELECTION, selectionArgs)
            return DeliveryResult(DeliveryCode.INJECTED)
        } finally {
            focused.recycle()
        }
    }

    companion object {
        @Volatile private var instance: BridgeAccessibilityService? = null
        @Volatile var availabilityListener: ((Boolean) -> Unit)? = null

        fun isReady(): Boolean = instance != null

        fun inject(value: String): DeliveryResult {
            val service = instance
                ?: return DeliveryResult(DeliveryCode.ACCESSIBILITY_OFF, "目标设备尚未启用声桥无障碍权限")
            if (value.isEmpty() || value.toByteArray(Charsets.UTF_8).size > 100 * 1024) {
                return DeliveryResult(DeliveryCode.INVALID, "内容为空或超过 100 KiB")
            }
            if (Looper.myLooper() == Looper.getMainLooper()) return service.injectOnMainThread(value)

            val result = AtomicReference<DeliveryResult>()
            val latch = CountDownLatch(1)
            Handler(Looper.getMainLooper()).post {
                result.set(service.injectOnMainThread(value))
                latch.countDown()
            }
            if (!latch.await(3, TimeUnit.SECONDS)) {
                return DeliveryResult(DeliveryCode.UNSUPPORTED_EDITOR, "无障碍服务响应超时")
            }
            return result.get()
        }

        private fun isPasswordInputType(inputType: Int): Boolean {
            val variation = inputType and InputType.TYPE_MASK_VARIATION
            return variation == InputType.TYPE_TEXT_VARIATION_PASSWORD ||
                variation == InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD ||
                variation == InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD ||
                variation == InputType.TYPE_NUMBER_VARIATION_PASSWORD
        }
    }
}
