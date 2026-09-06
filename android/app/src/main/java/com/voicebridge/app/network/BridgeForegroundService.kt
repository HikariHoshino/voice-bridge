package com.voicebridge.app.network

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.voicebridge.app.MainActivity
import com.voicebridge.app.R
import com.voicebridge.app.VoiceBridgeApplication

class BridgeForegroundService : Service() {
    private val repository get() = (application as VoiceBridgeApplication).repository

    override fun onCreate() {
        super.onCreate()
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "常驻接收", NotificationManager.IMPORTANCE_LOW).apply {
                description = "声桥在同一局域网中发现并接收已配对设备的文字"
                setShowBadge(false)
            },
        )
        val openIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val stopIntent = PendingIntent.getService(
            this,
            1,
            Intent(this, BridgeForegroundService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        startForeground(
            NOTIFICATION_ID,
            NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.stat_sys_upload_done)
                .setContentTitle("声桥正在接收")
                .setContentText("仅接受已配对设备的加密消息")
                .setContentIntent(openIntent)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .addAction(0, "停止", stopIntent)
                .build(),
        )
        repository.startNode()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    override fun onDestroy() {
        repository.stopNode()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        private const val CHANNEL_ID = "voicebridge_receive"
        private const val NOTIFICATION_ID = 2101
        private const val ACTION_STOP = "com.voicebridge.app.STOP_RECEIVING"
    }
}
