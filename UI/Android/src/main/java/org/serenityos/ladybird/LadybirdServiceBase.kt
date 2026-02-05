/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import android.app.Service
import android.content.Intent
import android.util.Log
import android.os.ParcelFileDescriptor
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import java.lang.ref.WeakReference
import java.util.concurrent.Executors

const val MSG_SET_RESOURCE_ROOT = 1
const val MSG_TRANSFER_SOCKET = 2

abstract class LadybirdServiceBase(protected val TAG: String) : Service() {
    private val threadPool = Executors.newCachedThreadPool()
    protected lateinit var resourceDir: String

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "Creating Service")
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.i(TAG, "Destroying Service")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.i(TAG, "Start command received")
        return super.onStartCommand(intent, flags, startId)
    }

    private fun handleTransferSockets(msg: Message) {
        val bundle = msg.data
        // FIXME: Handle garbage messages from wierd clients
        val ipcSocket = bundle.getParcelable<ParcelFileDescriptor>("IPC_SOCKET")!!
        createThread(ipcSocket)
    }

    private fun handleSetResourceRoot(msg: Message) {
        // FIXME: Handle this being already set, not being present, etc
        resourceDir = msg.data.getString("PATH")!!

        initNativeCode(resourceDir, TAG)
    }

    override fun onBind(p0: Intent?): IBinder? {
        // FIXME: Check the intent to make sure it's legit
        return Messenger(IncomingHandler(WeakReference(this))).binder
    }


    private fun createThread(ipcSocket: ParcelFileDescriptor) {
        threadPool.execute {
            nativeThreadLoop(ipcSocket.detachFd())
        }
    }

    private external fun nativeThreadLoop(ipcSocket: Int)
    private external fun initNativeCode(resourceDir: String, tagName: String);

    abstract fun handleServiceSpecificMessage(msg: Message): Boolean

    companion object {

        class IncomingHandler(private val service: WeakReference<LadybirdServiceBase>) :
            Handler(Looper.getMainLooper()) {
            override fun handleMessage(msg: Message) {
                when (msg.what) {
                    MSG_TRANSFER_SOCKET -> service.get()?.handleTransferSockets(msg)
                        ?: super.handleMessage(msg)

                    MSG_SET_RESOURCE_ROOT -> service.get()?.handleSetResourceRoot(msg)
                        ?: super.handleMessage(msg)

                    else -> {
                        val ret = service.get()?.handleServiceSpecificMessage(msg)
                        if (ret == null || !ret)
                            super.handleMessage(msg)
                    }
                }
            }
        }
    }
}
