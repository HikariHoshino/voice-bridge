package com.voicebridge.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.compose.setContent
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AccessibilityNew
import androidx.compose.material.icons.outlined.Computer
import androidx.compose.material.icons.outlined.DarkMode
import androidx.compose.material.icons.outlined.Devices
import androidx.compose.material.icons.outlined.LightMode
import androidx.compose.material.icons.outlined.PhoneAndroid
import androidx.compose.material.icons.outlined.Redo
import androidx.compose.material.icons.outlined.Send
import androidx.compose.material.icons.outlined.TabletAndroid
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.Typography
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.voicebridge.app.model.ConnectionState
import com.voicebridge.app.model.DeviceType
import com.voicebridge.app.model.Peer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    private val repository get() = (application as VoiceBridgeApplication).repository

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { VoiceBridgeScreen(repository) }
    }

    override fun onResume() {
        super.onResume()
        // Accessibility readiness is also updated by the service callback.
    }
}

private enum class ThemePreference { SYSTEM, LIGHT, DARK }

@Composable
private fun VoiceBridgeScreen(repository: BridgeRepository) {
    val state by repository.state.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val systemDark = androidx.compose.foundation.isSystemInDarkTheme()
    var theme by rememberSaveable { mutableStateOf(ThemePreference.SYSTEM) }
    val dark = theme == ThemePreference.DARK || (theme == ThemePreference.SYSTEM && systemDark)
    val background by animateColorAsState(if (dark) Color(0xFF0B1324) else Color(0xFFF5F7FB), tween(260), label = "background")
    val surface by animateColorAsState(if (dark) Color(0xFF152238) else Color.White, tween(260), label = "surface")
    val foreground by animateColorAsState(if (dark) Color(0xFFF1F6FF) else Color(0xFF10213A), tween(260), label = "foreground")
    val scheme = if (dark) {
        androidx.compose.material3.darkColorScheme(
            primary = Color(0xFF67A0FF), background = background, surface = surface,
            onBackground = foreground, onSurface = foreground,
        )
    } else {
        androidx.compose.material3.lightColorScheme(
            primary = Color(0xFF2367E8), background = background, surface = surface,
            onBackground = foreground, onSurface = foreground,
        )
    }

    val notificationPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) repository.startReceiving()
    }

    MaterialTheme(colorScheme = scheme, typography = Typography()) {
        Surface(Modifier.fillMaxSize(), color = background) {
            VoiceBridgeContent(
                repository = repository,
                state = state,
                surface = surface,
                theme = theme,
                onTheme = { theme = it },
                onReceiving = { enabled ->
                    if (!enabled) repository.stopReceiving()
                    else if (Build.VERSION.SDK_INT >= 33 &&
                        context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
                    ) notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
                    else repository.startReceiving()
                },
                onAccessibility = { context.startActivity(repository.accessibilitySettingsIntent()) },
            )
        }
    }

    state.pairPrompt?.let { prompt ->
        AlertDialog(
            onDismissRequest = { repository.confirmPair(false) },
            title = { Text(if (prompt.incoming) "收到配对请求" else "确认配对") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("请确认 ${prompt.peerName} 显示相同的六位短码：")
                    Text(prompt.code.chunked(3).joinToString(" "), fontSize = 32.sp, fontWeight = FontWeight.Bold)
                    Text("短码不同请立即取消。确认一次后，设备将在同一局域网自动重连。")
                }
            },
            confirmButton = { TextButton(onClick = { repository.confirmPair(true) }) { Text("短码一致") } },
            dismissButton = { TextButton(onClick = { repository.confirmPair(false) }) { Text("取消") } },
            shape = RoundedCornerShape(24.dp),
        )
    }
}

@Composable
private fun VoiceBridgeContent(
    repository: BridgeRepository,
    state: com.voicebridge.app.model.UiState,
    surface: Color,
    theme: ThemePreference,
    onTheme: (ThemePreference) -> Unit,
    onReceiving: (Boolean) -> Unit,
    onAccessibility: () -> Unit,
) {
    var draft by rememberSaveable(stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue()) }
    var undoSnapshot by remember { mutableStateOf<TextFieldValue?>(null) }
    val scope = rememberCoroutineScope()
    val selected = state.peers.firstOrNull { it.id == state.selectedPeerId }

    LazyColumn(
        modifier = Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(horizontal = 18.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item {
            Row(
                Modifier.fillMaxWidth().padding(top = 16.dp, bottom = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Surface(shape = RoundedCornerShape(16.dp), color = MaterialTheme.colorScheme.primary) {
                    Icon(Icons.Outlined.Devices, null, tint = Color.White, modifier = Modifier.padding(11.dp).size(28.dp))
                }
                Column(Modifier.padding(start = 13.dp).weight(1f)) {
                    Text("声桥", fontSize = 30.sp, fontWeight = FontWeight.Bold)
                    Text("${repository.identity.name} · ${repository.identity.type.label()}", color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                IconButton(onClick = {
                    onTheme(when (theme) {
                        ThemePreference.SYSTEM -> ThemePreference.LIGHT
                        ThemePreference.LIGHT -> ThemePreference.DARK
                        ThemePreference.DARK -> ThemePreference.SYSTEM
                    })
                }) {
                    Icon(if (theme == ThemePreference.DARK) Icons.Outlined.DarkMode else Icons.Outlined.LightMode, "切换主题")
                }
            }
        }

        item {
            RoundedCard(surface) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("常驻接收", fontWeight = FontWeight.SemiBold, fontSize = 18.sp)
                        Text(if (state.receiving) "同一局域网内可发现本设备" else "开启后无需电脑也能互传", color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    Switch(checked = state.receiving, onCheckedChange = onReceiving)
                }
                Spacer(Modifier.height(10.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(Icons.Outlined.AccessibilityNew, null, tint = if (state.accessibilityReady) Color(0xFF23B47E) else MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(
                        if (state.accessibilityReady) "光标注入已授权" else "光标注入未授权",
                        Modifier.padding(start = 8.dp).weight(1f),
                    )
                    OutlinedButton(onClick = onAccessibility, shape = RoundedCornerShape(16.dp)) { Text("系统设置") }
                }
            }
        }

        item { Text("目标设备", fontSize = 18.sp, fontWeight = FontWeight.SemiBold) }
        if (state.peers.isEmpty()) {
            item {
                RoundedCard(surface) {
                    Text(if (state.receiving) "正在自动发现同一局域网中的声桥设备……" else "请先开启常驻接收")
                }
            }
        } else {
            items(state.peers, key = { it.id }) { peer ->
                PeerCard(peer, selected = peer.id == state.selectedPeerId, onClick = { repository.selectPeer(peer.id) }, onPair = { repository.requestPair(peer.id) })
            }
        }

        item {
            RoundedCard(surface) {
                Text("发送文字", fontWeight = FontWeight.SemiBold, fontSize = 18.sp)
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = draft,
                    onValueChange = { draft = it },
                    modifier = Modifier.fillMaxWidth().height(190.dp),
                    placeholder = { Text("用你现有的输入法在这里输入……") },
                    shape = RoundedCornerShape(18.dp),
                )
                Spacer(Modifier.height(12.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    OutlinedButton(
                        onClick = { undoSnapshot?.let { draft = it; undoSnapshot = null } },
                        enabled = undoSnapshot != null,
                        shape = RoundedCornerShape(16.dp),
                    ) {
                        Icon(Icons.Outlined.Redo, null)
                        Text("撤销", Modifier.padding(start = 6.dp))
                    }
                    Button(
                        onClick = {
                            val target = selected ?: return@Button
                            val sentSnapshot = draft
                            scope.launch {
                                val result = withContext(Dispatchers.IO) { repository.sendText(target.id, sentSnapshot.text) }
                                if (result.delivered) {
                                    undoSnapshot = sentSnapshot
                                    draft = TextFieldValue("")
                                }
                            }
                        },
                        enabled = !state.busy && draft.text.isNotEmpty() && selected?.state == ConnectionState.CONNECTED,
                        modifier = Modifier.weight(1f),
                        shape = RoundedCornerShape(16.dp),
                    ) {
                        if (state.busy) CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                        else Icon(Icons.Outlined.Send, null)
                        Text("发送到 ${selected?.name ?: "所选设备"}", Modifier.padding(start = 7.dp))
                    }
                }
            }
        }

        item {
            Surface(color = Color(0x1923B47E), shape = RoundedCornerShape(16.dp), modifier = Modifier.fillMaxWidth()) {
                Text(state.status, Modifier.padding(horizontal = 14.dp, vertical = 12.dp), color = Color(0xFF23B47E))
            }
            Spacer(Modifier.height(14.dp))
        }
    }
}

@Composable
private fun RoundedCard(color: Color, content: @Composable ColumnScope.() -> Unit) {
    Card(
        colors = CardDefaults.cardColors(containerColor = color),
        shape = RoundedCornerShape(22.dp),
        modifier = Modifier.fillMaxWidth(),
    ) { Column(Modifier.padding(18.dp), content = content) }
}

@Composable
private fun PeerCard(peer: Peer, selected: Boolean, onClick: () -> Unit, onPair: () -> Unit) {
    val icon: ImageVector = when (peer.type) {
        DeviceType.PHONE -> Icons.Outlined.PhoneAndroid
        DeviceType.TABLET -> Icons.Outlined.TabletAndroid
        DeviceType.COMPUTER -> Icons.Outlined.Computer
    }
    Card(
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick),
        shape = RoundedCornerShape(19.dp),
        border = if (selected) BorderStroke(2.dp, MaterialTheme.colorScheme.primary) else null,
    ) {
        Row(Modifier.padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
            Surface(shape = CircleShape, color = MaterialTheme.colorScheme.primaryContainer) {
                Icon(icon, null, Modifier.padding(10.dp).size(24.dp), tint = MaterialTheme.colorScheme.primary)
            }
            Column(Modifier.padding(start = 12.dp).weight(1f)) {
                Text(peer.name, fontWeight = FontWeight.SemiBold)
                Text(peer.state.label(), color = peer.state.color(), fontSize = 13.sp)
            }
            if (!peer.trusted) OutlinedButton(onClick = onPair, shape = RoundedCornerShape(14.dp)) { Text("配对") }
        }
    }
}

private fun DeviceType.label() = when (this) {
    DeviceType.PHONE -> "手机"
    DeviceType.TABLET -> "平板"
    DeviceType.COMPUTER -> "电脑"
}

private fun ConnectionState.label() = when (this) {
    ConnectionState.DISCOVERED -> "已发现"
    ConnectionState.UNPAIRED -> "未配对"
    ConnectionState.PAIRING -> "正在配对"
    ConnectionState.CONNECTING -> "正在连接"
    ConnectionState.CONNECTED -> "已连接"
    ConnectionState.RETRYING -> "连接已断开，正在重试"
    ConnectionState.PAUSED -> "接收已暂停"
}

@Composable
private fun ConnectionState.color() = when (this) {
    ConnectionState.CONNECTED -> Color(0xFF23B47E)
    ConnectionState.RETRYING -> Color(0xFFE79528)
    else -> MaterialTheme.colorScheme.onSurfaceVariant
}
