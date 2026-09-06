package com.voicebridge.app

import android.content.Context
import android.content.Intent
import android.provider.Settings
import com.voicebridge.app.accessibility.BridgeAccessibilityService
import com.voicebridge.app.model.ConnectionState
import com.voicebridge.app.model.DeliveryCode
import com.voicebridge.app.model.DeliveryResult
import com.voicebridge.app.model.PairPrompt
import com.voicebridge.app.model.Peer
import com.voicebridge.app.model.UiState
import com.voicebridge.app.network.BridgeForegroundService
import com.voicebridge.app.network.NsdDiscovery
import com.voicebridge.app.network.PeerClient
import com.voicebridge.app.network.PeerServer
import com.voicebridge.app.storage.DeviceIdentity
import com.voicebridge.app.storage.TrustStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeUnit

class BridgeRepository(private val context: Context) : PeerServer.Callback {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    val identity = DeviceIdentity(context)
    private val trustStore = TrustStore(context)
    private val client = PeerClient(identity, trustStore)
    private val _state = MutableStateFlow(UiState(accessibilityReady = BridgeAccessibilityService.isReady()))
    val state: StateFlow<UiState> = _state.asStateFlow()
    private var server: PeerServer? = null
    private var discovery: NsdDiscovery? = null
    private var heartbeatJob: Job? = null
    private val failures = mutableMapOf<String, Int>()
    private val peerLock = Any()
    @Volatile private var pairDecision: CompletableFuture<Boolean>? = null

    init {
        BridgeAccessibilityService.availabilityListener = { ready -> update { copy(accessibilityReady = ready) } }
    }

    fun startReceiving() {
        context.startForegroundService(Intent(context, BridgeForegroundService::class.java))
    }

    fun stopReceiving() {
        context.stopService(Intent(context, BridgeForegroundService::class.java))
    }

    fun startNode() {
        if (server != null) return
        val newServer = PeerServer(identity, trustStore, this).also { it.start() }
        server = newServer
        discovery = NsdDiscovery(context, identity, newServer.port, ::peerFound, ::peerLost).also { it.start() }
        update { copy(receiving = true, status = "正在发现局域网设备") }
        heartbeatJob = scope.launch { heartbeatLoop() }
    }

    fun stopNode() {
        heartbeatJob?.cancel()
        heartbeatJob = null
        discovery?.stop()
        discovery = null
        server?.stop()
        server = null
        synchronized(peerLock) { failures.clear() }
        update { copy(receiving = false, peers = emptyList(), selectedPeerId = null, status = "常驻接收未开启") }
    }

    fun selectPeer(peerId: String) = update { copy(selectedPeerId = peerId) }

    fun requestPair(peerId: String) {
        val peer = _state.value.peers.firstOrNull { it.id == peerId } ?: return
        updatePeer(peerId) { copy(state = ConnectionState.PAIRING) }
        scope.launch {
            val result = runCatching {
                client.pair(peer) { code -> awaitPairDecision(peer.id, peer.name, code, incoming = false) }
            }.getOrElse { DeliveryResult(DeliveryCode.UNREACHABLE, it.message ?: "配对连接失败") }
            update {
                val current = this
                current.copy(
                    status = result.detail.ifBlank { if (result.delivered) "配对成功" else "配对失败" },
                    peers = current.peers.map {
                        if (it.id != peerId) it else it.copy(
                            trusted = result.delivered,
                            state = if (result.delivered) ConnectionState.CONNECTING else ConnectionState.UNPAIRED,
                        )
                    },
                )
            }
        }
    }

    fun confirmPair(approved: Boolean) {
        pairDecision?.complete(approved)
        pairDecision = null
        update { copy(pairPrompt = null) }
    }

    suspend fun sendText(peerId: String, text: String): DeliveryResult {
        val peer = _state.value.peers.firstOrNull { it.id == peerId }
            ?: return DeliveryResult(DeliveryCode.UNREACHABLE, "目标设备不在当前局域网")
        update { copy(busy = true, status = "正在发送到 ${peer.name}") }
        val result = client.sendText(peer, text)
        update {
            copy(
                busy = false,
                status = if (result.delivered) "已写入 ${peer.name} 的当前光标" else result.detail,
            )
        }
        return result
    }

    fun accessibilitySettingsIntent(): Intent = Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)

    override fun approvePair(peerId: String, peerName: String, fingerprint: String, code: String): Boolean =
        awaitPairDecision(peerId, peerName, code, incoming = true)

    override fun inject(text: String): DeliveryResult = BridgeAccessibilityService.inject(text)
    override fun isReceiving(): Boolean = _state.value.receiving
    override fun onAuthenticatedActivity(peerId: String) {
        if (peerId.isNotBlank()) updatePeer(peerId) {
            copy(state = ConnectionState.CONNECTED, lastHeartbeatMillis = System.currentTimeMillis())
        }
    }

    private fun awaitPairDecision(peerId: String, peerName: String, code: String, incoming: Boolean): Boolean {
        val decision = CompletableFuture<Boolean>()
        pairDecision?.complete(false)
        pairDecision = decision
        update { copy(pairPrompt = PairPrompt(peerId, peerName, code, incoming), status = "请核对配对短码") }
        return runCatching { decision.get(60, TimeUnit.SECONDS) }.getOrDefault(false)
    }

    private fun peerFound(discovered: Peer) {
        val trusted = trustStore.get(discovered.id)
        val isTrusted = trusted != null && ProtocolFingerprint.matches(trusted.fingerprint, discovered.fingerprint)
        update {
            val current = this
            val existing = current.peers.firstOrNull { it.id == discovered.id }
            val peer = discovered.copy(
                trusted = isTrusted,
                state = when {
                    !isTrusted -> ConnectionState.UNPAIRED
                    existing?.state == ConnectionState.CONNECTED -> ConnectionState.CONNECTED
                    else -> ConnectionState.CONNECTING
                },
                lastHeartbeatMillis = existing?.lastHeartbeatMillis ?: 0,
            )
            val list = current.peers.filterNot { it.id == peer.id } + peer
            current.copy(
                peers = list.sortedBy { it.name.lowercase() },
                selectedPeerId = current.selectedPeerId ?: peer.id,
                status = if (list.isEmpty()) "未发现其他声桥设备" else "发现 ${list.size} 台设备",
            )
        }
    }

    private fun peerLost(peerId: String) = updatePeer(peerId) { copy(state = ConnectionState.RETRYING) }

    private suspend fun heartbeatLoop() {
        while (scope.isActive) {
            val trustedPeers = _state.value.peers.filter { it.trusted }
            trustedPeers.forEach { peer ->
                val result = client.ping(peer)
                synchronized(peerLock) {
                    if (result.delivered) {
                        failures[peer.id] = 0
                        updatePeer(peer.id) {
                            copy(state = ConnectionState.CONNECTED, lastHeartbeatMillis = System.currentTimeMillis())
                        }
                    } else {
                        val count = (failures[peer.id] ?: 0) + 1
                        failures[peer.id] = count
                        if (count >= 3) updatePeer(peer.id) { copy(state = ConnectionState.RETRYING) }
                    }
                }
            }
            delay(5_000)
        }
    }

    private fun updatePeer(peerId: String, transform: Peer.() -> Peer) = update {
        val current = this
        current.copy(peers = current.peers.map { if (it.id == peerId) it.transform() else it })
    }

    private fun update(transform: UiState.() -> UiState) {
        synchronized(_state) { _state.value = _state.value.transform() }
    }

    private object ProtocolFingerprint {
        fun matches(first: String, second: String) =
            java.security.MessageDigest.isEqual(first.toByteArray(), second.toByteArray())
    }
}
