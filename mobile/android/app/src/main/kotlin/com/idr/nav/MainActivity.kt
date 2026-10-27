package com.idr.nav

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {

    private val NAV_CHANNEL = "idr/nav"
    private val STATE_CHANNEL = "idr/state"
    private val EVENT_CHANNEL = "idr/events"

    private var navService: NavService? = null
    private var isBound = false
    private var pendingStartConfig: Triple<String, String, String>? = null

    private var stateSink: EventChannel.EventSink? = null
    private var eventSink: EventChannel.EventSink? = null

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(className: ComponentName, service: IBinder) {
            val binder = service as NavService.LocalBinder
            navService = binder.getService()
            isBound = true

            navService?.onStateUpdate = { st ->
                runOnUiThread {
                    val map = HashMap<String, Any>()
                    map["t_ns"] = st[0].toLong()
                    map["lat"] = st[1]
                    map["lon"] = st[2]
                    map["alt"] = st[3]
                    map["speed_mps"] = st[4]
                    map["heading_deg"] = st[5]
                    map["mode"] = st[6].toInt()
                    map["vehicle_mode"] = st[7].toInt()
                    map["hpl_m"] = st[8]
                    map["ell_major_m"] = st[9]
                    map["ell_minor_m"] = st[10]
                    map["way_id"] = st[11].toLong()

                    stateSink?.success(map)
                }
            }

            navService?.onEventUpdate = { type, payload ->
                runOnUiThread {
                    val map = HashMap<String, Any>()
                    map["type"] = type
                    map["payload"] = payload
                    eventSink?.success(map)
                }
            }

            pendingStartConfig?.let { (speedModel, adapterModel, mapDb) ->
                navService?.startNavigation(speedModel, adapterModel, mapDb)
                pendingStartConfig = null
            }
        }

        override fun onServiceDisconnected(arg0: ComponentName) {
            isBound = false
            navService = null
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        // MethodChannel
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, NAV_CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "start" -> {
                    val speedModel = call.argument<String>("speedModel") ?: ""
                    val adapterModel = call.argument<String>("adapterModel") ?: ""
                    val mapDb = call.argument<String>("mapDb") ?: ""

                    if (isBound && navService != null) {
                        navService?.startNavigation(speedModel, adapterModel, mapDb)
                    } else {
                        pendingStartConfig = Triple(speedModel, adapterModel, mapDb)
                        val intent = Intent(this, NavService::class.java)
                        startService(intent)
                        bindService(intent, connection, Context.BIND_AUTO_CREATE)
                    }
                    result.success(true)
                }
                "stop" -> {
                    pendingStartConfig = null
                    navService?.stopNavigation()
                    if (isBound) {
                        unbindService(connection)
                        isBound = false
                    }
                    result.success(true)
                }
                else -> result.notImplemented()
            }
        }

        // EventChannels
        EventChannel(flutterEngine.dartExecutor.binaryMessenger, STATE_CHANNEL).setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    stateSink = events
                }
                override fun onCancel(arguments: Any?) {
                    stateSink = null
                }
            }
        )

        EventChannel(flutterEngine.dartExecutor.binaryMessenger, EVENT_CHANNEL).setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    eventSink = events
                }
                override fun onCancel(arguments: Any?) {
                    eventSink = null
                }
            }
        )
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isBound) {
            unbindService(connection)
            isBound = false
        }
    }
}
