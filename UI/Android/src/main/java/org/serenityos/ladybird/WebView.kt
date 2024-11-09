/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View

// FIXME: This should (eventually) implement NestedScrollingChild3 and ScrollingView
class WebView(context: Context, attributeSet: AttributeSet) : View(context, attributeSet) {
    private val viewImpl = WebViewImplementation(this)
    private lateinit var contentBitmap: Bitmap
    var onLoadStart: (url: String, isRedirect: Boolean) -> Unit = { _, _ -> }

    fun initialize(resourceDir: String) {
        viewImpl.initialize(resourceDir)
    }

    fun dispose() {
        viewImpl.dispose()
    }

    fun loadURL(url: String) {
        viewImpl.loadURL(url)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        // The native side only supports down, move, and up events.
        // So, ignore any other MotionEvents.
        if (event.action != MotionEvent.ACTION_DOWN &&
            event.action != MotionEvent.ACTION_MOVE &&
            event.action != MotionEvent.ACTION_UP) {
            return super.onTouchEvent(event);
        }

        // FIXME: We are passing these through as mouse events.
        // We should really be handling them as touch events.
        // (And we should handle scrolling - right now you have tap and drag the scrollbar!)
        viewImpl.mouseEvent(event.action, event.x, event.y, event.rawX, event.rawY)

        return true
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        contentBitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)

        val pixelDensity = context.resources.displayMetrics.density
        viewImpl.setDevicePixelRatio(pixelDensity)

        // FIXME: Account for scroll offset when view supports scrolling
        viewImpl.setViewportGeometry(w, h)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        viewImpl.drawIntoBitmap(contentBitmap);
        canvas.drawBitmap(contentBitmap, 0f, 0f, null)
    }

}
