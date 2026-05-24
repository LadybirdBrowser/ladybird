/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import android.content.Context
import android.content.Intent
import android.os.Message
import android.util.Log

class WebContentService : LadybirdServiceBase("WebContentService") {
    override fun handleServiceSpecificMessage(msg: Message): Boolean {
        return false
    }

    init {
        nativeInit();
    }

    private fun bindRequestServer(ipcFd: Int)
    {
        Log.i(TAG, "Binding RequestServer with IPC fd $ipcFd")
        val connector = LadybirdServiceConnection(ipcFd, resourceDir)
        connector.onDisconnect = {
            // FIXME: Notify impl that service is dead and might need restarted
            Log.e(TAG, "RequestServer Died! :(")
        }
        // FIXME: Unbind this at some point maybe
        val bound = bindService(
            Intent(this, RequestServerService::class.java),
            connector,
            Context.BIND_AUTO_CREATE
        )
        Log.i(TAG, "bindService(RequestServerService) returned $bound")
    }

    private fun bindImageDecoder(ipcFd: Int)
    {
        Log.i(TAG, "Binding ImageDecoder with IPC fd $ipcFd")
        val connector = LadybirdServiceConnection(ipcFd, resourceDir)
        connector.onDisconnect = {
            // FIXME: Notify impl that service is dead and might need restarted
            Log.e(TAG, "ImageDecoder Died! :(")
        }
        // FIXME: Unbind this at some point maybe
        val bound = bindService(
            Intent(this, ImageDecoderService::class.java),
            connector,
            Context.BIND_AUTO_CREATE
        )
        Log.i(TAG, "bindService(ImageDecoderService) returned $bound")
    }

    private fun bindWebWorker(ipcFd: Int)
    {
        Log.i(TAG, "Binding WebWorker with IPC fd $ipcFd")
        val connector = LadybirdServiceConnection(ipcFd, resourceDir)
        connector.onDisconnect = {
            Log.e(TAG, "WebWorker Died! :(")
        }
        val bound = bindService(
            Intent(this, WebWorkerService::class.java),
            connector,
            Context.BIND_AUTO_CREATE
        )
        Log.i(TAG, "bindService(WebWorkerService) returned $bound")
    }

    external fun nativeInit()

    companion object {
        init {
            System.loadLibrary("webcontentservice")
        }
    }
}
