package com.voicebridge.app

import android.app.Application

class VoiceBridgeApplication : Application() {
    val repository: BridgeRepository by lazy { BridgeRepository(applicationContext) }
}
