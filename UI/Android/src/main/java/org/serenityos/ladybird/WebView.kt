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
import android.view.ViewConfiguration
import kotlin.math.abs
import kotlin.math.roundToInt

// FIXME: This should (eventually) implement NestedScrollingChild3 and ScrollingView
class WebView(context: Context, attributeSet: AttributeSet) : View(context, attributeSet) {
    private val viewImpl = WebViewImplementation(this)
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private lateinit var contentBitmap: Bitmap
    private var downX = 0f
    private var downY = 0f
    private var lastX = 0f
    private var lastY = 0f
    private var isScrollingGesture = false
    var onLoadStart: (url: String, isRedirect: Boolean) -> Unit = { _, _ -> }
    var onLoadFinish: (url: String) -> Unit = { }
    var onTitleChange: (title: String) -> Unit = { }
    var onUrlChange: (url: String) -> Unit = { }
    var onFindInPage: (current: Int, total: Int) -> Unit = { _, _ -> }
    var onLinkHover: (url: String?) -> Unit = { }
    var onContentReady: () -> Unit = { }
    var onWebContentCrash: () -> Unit = { }

    fun initialize(resourceDir: String) {
        viewImpl.initialize(resourceDir)
    }

    fun dispose() {
        viewImpl.dispose()
    }

    fun loadURL(url: String) {
        viewImpl.loadURL(url)
    }

    fun reload() {
        viewImpl.reload()
    }

    fun goBack() {
        viewImpl.goBack()
    }

    fun goForward() {
        viewImpl.goForward()
    }

    fun findInPage(query: String, caseSensitive: Boolean = false) = viewImpl.findInPage(query, caseSensitive)
    fun findNext() = viewImpl.findNext()
    fun findPrevious() = viewImpl.findPrevious()
    fun zoomIn() = viewImpl.zoomIn().also { syncViewport() }
    fun zoomOut() = viewImpl.zoomOut().also { syncViewport() }
    fun zoomReset() = viewImpl.zoomReset().also { syncViewport() }
    fun zoomLevel(): Double = viewImpl.zoomLevel()
    fun setPreferredColorScheme(scheme: Int) = viewImpl.setPreferredColorScheme(scheme)
    fun runJavascript(js: String) = viewImpl.runJavascript(js)
    fun selectAllOnPage() = viewImpl.selectAllOnPage()

    /** Re-emit the current viewport size and pixel ratio to WebContent. */
    fun syncViewport() {
        if (width <= 0 || height <= 0) return
        val pixelDensity = context.resources.displayMetrics.density
        viewImpl.setDevicePixelRatio(pixelDensity)
        viewImpl.setViewportGeometry(width, height)
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.pointerCount > 1)
            return true

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x
                downY = event.y
                lastX = event.x
                lastY = event.y
                isScrollingGesture = false
                parent?.requestDisallowInterceptTouchEvent(true)
                return true
            }

            MotionEvent.ACTION_MOVE -> {
                val totalDx = event.x - downX
                val totalDy = event.y - downY
                if (!isScrollingGesture && (abs(totalDx) > touchSlop || abs(totalDy) > touchSlop))
                    isScrollingGesture = true

                if (isScrollingGesture) {
                    val stepDx = event.x - lastX
                    val stepDy = event.y - lastY
                    val wheelDx = (-stepDx * SCROLL_MULTIPLIER).roundToInt()
                    val wheelDy = (-stepDy * SCROLL_MULTIPLIER).roundToInt()
                    if (wheelDx != 0 || wheelDy != 0)
                        viewImpl.wheelEvent(event.x, event.y, event.rawX, event.rawY, wheelDx, wheelDy)
                }

                lastX = event.x
                lastY = event.y
                return true
            }

            MotionEvent.ACTION_UP -> {
                if (!isScrollingGesture) {
                    viewImpl.mouseEvent(MotionEvent.ACTION_DOWN, event.x, event.y, event.rawX, event.rawY)
                    viewImpl.mouseEvent(MotionEvent.ACTION_UP, event.x, event.y, event.rawX, event.rawY)
                    performClick()
                }
                isScrollingGesture = false
                return true
            }

            MotionEvent.ACTION_CANCEL -> {
                isScrollingGesture = false
                return true
            }

            else -> {
                return super.onTouchEvent(event)
            }
        }
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

    companion object {
        private const val SCROLL_MULTIPLIER = 2.25f
    }

}
