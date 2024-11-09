/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import android.content.ComponentName
import android.content.ServiceConnection
import android.os.IBinder
import android.os.Message
import android.os.Messenger
import android.os.ParcelFileDescriptor

class LadybirdServiceConnection(
    private var ipcFd: Int,
    private var resourceDir: String
) :
    ServiceConnection {
    var boundToService: Boolean = false
    var onDisconnect: () -> Unit = {}
    private var service: Messenger? = null

    override fun onServiceConnected(className: ComponentName, svc: IBinder) {
        // This is called when the connection with the service has been
        // established, giving us the object we can use to
        // interact with the service.  We are communicating with the
        // service using a Messenger, so here we get a client-side
        // representation of that from the raw IBinder object.
        service = Messenger(svc)
        boundToService = true

        val init = Message.obtain(null, MSG_SET_RESOURCE_ROOT)
        init.data.putString("PATH", resourceDir)
        service!!.send(init)

        val parcel = ParcelFileDescriptor.adoptFd(ipcFd)
        val msg = Message.obtain(null, MSG_TRANSFER_SOCKET)
        msg.data.putParcelable("IPC_SOCKET", parcel)
        service!!.send(msg)
        parcel.detachFd()
    }

    override fun onServiceDisconnected(className: ComponentName) {
        // This is called when the connection with the service has been
        // unexpectedly disconnected; that is, its process crashed.
        service = null
        boundToService = false

        // Notify owner that the service is dead
        onDisconnect()
    }
}
