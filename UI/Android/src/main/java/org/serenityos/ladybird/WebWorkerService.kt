/**
 * Copyright (c) 2025, Ladybird Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import android.os.Message

class WebWorkerService : LadybirdServiceBase("WebWorkerService") {
    override fun handleServiceSpecificMessage(msg: Message): Boolean = false

    companion object {
        init {
            System.loadLibrary("webworkerservice")
        }
    }
}
