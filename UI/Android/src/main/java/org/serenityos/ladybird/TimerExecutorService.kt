/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

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
        val id = nextId.incrementAndGet()
        timer.id = id

        val handle: ScheduledFuture<*> = if (singleShot) {
            executor.schedule({
                try {
                    timer.run()
                } finally {
                    timers.remove(id)
                }
            }, milliseconds, TimeUnit.MILLISECONDS)
        } else {
            executor.scheduleWithFixedDelay(timer, milliseconds, milliseconds, TimeUnit.MILLISECONDS)
        }
        timers[id] = handle
        return id
    }

    fun unregisterTimer(id: Long) {
        val timer = timers.remove(id) ?: return
        timer.cancel(false)
    }

    private val nextId = AtomicLong(0)
    private val timers = java.util.concurrent.ConcurrentHashMap<Long, ScheduledFuture<*>>()

}
