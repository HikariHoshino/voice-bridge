package com.voicebridge.app.model

enum class DeviceType(val wireName: String) {
    PHONE("phone"), TABLET("tablet"), COMPUTER("computer");

    companion object {
        fun fromWire(value: String): DeviceType = entries.firstOrNull { it.wireName == value } ?: PHONE
    }
}

enum class ConnectionState {
    DISCOVERED, UNPAIRED, PAIRING, CONNECTING, CONNECTED, RETRYING, PAUSED
}

data class Peer(
    val id: String,
    val name: String,
    val type: DeviceType,
    val host: String,
    val port: Int,
    val fingerprint: String,
    val trusted: Boolean,
    val state: ConnectionState,
    val lastSeenMillis: Long = 0,
    val lastHeartbeatMillis: Long = 0,
)

enum class DeliveryCode(val wireName: String) {
    INJECTED("injected"),
    NO_FOCUS("no_focus"),
    ACCESSIBILITY_OFF("accessibility_off"),
    PASSWORD_FIELD("password_field"),
    UNSUPPORTED_EDITOR("unsupported_editor"),
    PAUSED("paused"),
    INVALID("invalid"),
    UNAUTHORIZED("unauthorized"),
    UNREACHABLE("unreachable");

    companion object {
        fun fromWire(value: String): DeliveryCode = entries.firstOrNull { it.wireName == value } ?: INVALID
    }
}

data class DeliveryResult(val code: DeliveryCode, val detail: String = "") {
    val delivered: Boolean get() = code == DeliveryCode.INJECTED
}

data class PairPrompt(
    val peerId: String,
    val peerName: String,
    val code: String,
    val incoming: Boolean,
)

data class UiState(
    val peers: List<Peer> = emptyList(),
    val selectedPeerId: String? = null,
    val receiving: Boolean = false,
    val accessibilityReady: Boolean = false,
    val status: String = "常驻接收未开启",
    val pairPrompt: PairPrompt? = null,
    val busy: Boolean = false,
)
