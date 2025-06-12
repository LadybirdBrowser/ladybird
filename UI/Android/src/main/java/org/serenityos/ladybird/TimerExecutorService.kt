/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

class TimerExecutorService {

    private val executor = Executors.newSingleThreadScheduledExecutor()

    class Timer(private var nativeData: Long) : Runnable {
        override fun run() {
            nativeRun(nativeData, id)
        }

        private external fun nativeRun(nativeData: Long, id: Long)
        var id: Long = 0
    }

    fun registerTimer(timer: Timer, singleShot: Boolean, milliseconds: Long): Long {
        val id = ++nextId
        timer.id = id
        val handle: ScheduledFuture<*> = if (singleShot) executor.schedule(
            timer,
            milliseconds,
            TimeUnit.MILLISECONDS
        ) else executor.scheduleWithFixedDelay(
            timer,
            milliseconds,
            milliseconds,
            TimeUnit.MILLISECONDS
        )
        timers[id] = handle
        return id
    }

    fun unregisterTimer(id: Long) {
        val timer = timers[id] ?: return
        timer.cancel(false)
    }

    private var nextId: Long = 0
    private val timers: HashMap<Long, ScheduledFuture<*>> = hashMapOf()

}
