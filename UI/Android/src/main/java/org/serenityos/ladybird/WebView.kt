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
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import android.view.ViewConfiguration
import android.widget.OverScroller
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
    private var isScalingGesture = false
    private var pinchZoomEnabled = true
    private val flinger = OverScroller(context)
    private var lastFlingX = 0
    private var lastFlingY = 0
    var onLoadStart: (url: String, isRedirect: Boolean) -> Unit = { _, _ -> }
    var onLoadFinish: (url: String) -> Unit = { }
    var onTitleChange: (title: String) -> Unit = { }
    var onUrlChange: (url: String) -> Unit = { }
    var onFindInPage: (current: Int, total: Int) -> Unit = { _, _ -> }
    var onLinkHover: (url: String?) -> Unit = { }
    var onContentReady: () -> Unit = { }
    var onWebContentCrash: () -> Unit = { }
    var onLongPress: (x: Float, y: Float) -> Unit = { _, _ -> }
    var onSwipeRefresh: () -> Unit = { }

    private val scaleDetector = ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        private var accumulated = 0.0

        override fun onScaleBegin(detector: ScaleGestureDetector): Boolean {
            if (!pinchZoomEnabled) return false
            accumulated = 0.0
            isScalingGesture = true
            flinger.forceFinished(true)
            return true
        }

        override fun onScale(detector: ScaleGestureDetector): Boolean {
            // Accumulate scale factor and apply discrete zoom in/out steps so we
            // stay aligned with the engine's preferred zoom ladder.
            accumulated += (detector.scaleFactor - 1.0)
            while (accumulated > 0.10) {
                viewImpl.zoomIn(); accumulated -= 0.10
            }
            while (accumulated < -0.10) {
                viewImpl.zoomOut(); accumulated += 0.10
            }
            return true
        }

        override fun onScaleEnd(detector: ScaleGestureDetector) {
            isScalingGesture = false
        }
    }).apply {
        isQuickScaleEnabled = false
    }

    private val gestureDetector = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onLongPress(e: MotionEvent) {
            // Forward to the activity so it can show a contextual menu near
            // the touch. We also synthesize a select-word on the page.
            onLongPress(e.x, e.y)
        }

        override fun onFling(
            e1: MotionEvent?,
            e2: MotionEvent,
            velocityX: Float,
            velocityY: Float
        ): Boolean {
            if (isScalingGesture) return false
            flinger.forceFinished(true)
            lastFlingX = 0
            lastFlingY = 0
            // Velocity is in px/s; flip sign because page scrolls opposite to swipe.
            flinger.fling(
                0, 0,
                (-velocityX).toInt(), (-velocityY).toInt(),
                Int.MIN_VALUE, Int.MAX_VALUE,
                Int.MIN_VALUE, Int.MAX_VALUE
            )
            this@WebView.postInvalidateOnAnimation()
            return true
        }

        override fun onDoubleTap(e: MotionEvent): Boolean {
            // Quick zoom toggle like mobile Chromium.
            val lvl = viewImpl.zoomLevel()
            if (lvl > 1.01) viewImpl.zoomReset() else viewImpl.zoomIn()
            return true
        }
    }).apply {
        setIsLongpressEnabled(true)
    }


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

    fun setUserAgent(preset: UserAgentPreset) {
        // Keep navigator.platform in sync with the spoofed UA so sites don't see
        // an Android platform string paired with a desktop Chrome UA.
        viewImpl.debugRequest("platform", preset.platformString ?: "")
        // Pass the full UA string as argument; null/empty means "reset to default".
        viewImpl.debugRequest("spoof-user-agent", preset.uaString ?: "")
    }

    fun setNavigatorCompatibility(mode: NavigatorCompatibility) {
        viewImpl.debugRequest("navigator-compatibility-mode", mode.nativeName)
    }

    fun setScriptingEnabled(enabled: Boolean) {
        viewImpl.debugRequest("scripting", if (enabled) "on" else "off")
    }

    fun clearCache() {
        viewImpl.debugRequest("clear-cache")
    }

    fun collectGarbage() {
        viewImpl.debugRequest("collect-garbage")
    }

    fun setPinchZoomEnabled(enabled: Boolean) {
        pinchZoomEnabled = enabled
    }

    fun stopScrolling() {
        flinger.forceFinished(true)
    }

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
        scaleDetector.onTouchEvent(event)
        gestureDetector.onTouchEvent(event)

        // Multi-touch (typically pinch) is consumed by the scale detector;
        // don't double-dispatch it as a scroll.
        if (event.pointerCount > 1) {
            isScrollingGesture = false
            parent?.requestDisallowInterceptTouchEvent(true)
            return true
        }
        if (isScalingGesture)
            return true

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                flinger.forceFinished(true)
                downX = event.x
                downY = event.y
                lastX = event.x
                lastY = event.y
                isScrollingGesture = false
                parent?.requestDisallowInterceptTouchEvent(true)
                viewImpl.mouseEvent(MotionEvent.ACTION_MOVE, event.x, event.y, event.rawX, event.rawY)
                return true
            }

            MotionEvent.ACTION_MOVE -> {
                val totalDx = event.x - downX
                val totalDy = event.y - downY
                if (!isScrollingGesture && (abs(totalDx) > touchSlop || abs(totalDy) > touchSlop))
                    isScrollingGesture = true

                if (isScrollingGesture) {
                    // 1:1 pixel delta — match what a touch device would scroll
                    // natively. The previous 5x multiplier overshot massively
                    // and could leave the page stuck against the top/bottom
                    // boundary which made it seem only the corners worked.
                    val stepDx = event.x - lastX
                    val stepDy = event.y - lastY
                    val wheelDx = (-stepDx).roundToInt()
                    val wheelDy = (-stepDy).roundToInt()
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
                } else {
                    // If user swiped down from the very top while not scrolled,
                    // emit a pull-to-refresh signal. Real overscroll detection
                    // would require knowing the current scroll position from
                    // the engine; this is a useful approximation.
                    val totalDy = event.y - downY
                    if (totalDy > resources.displayMetrics.density * 96 && abs(event.x - downX) < totalDy && downY < resources.displayMetrics.density * 80)
                        onSwipeRefresh()
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

    override fun computeScroll() {
        if (flinger.computeScrollOffset()) {
            val x = flinger.currX
            val y = flinger.currY
            val dx = x - lastFlingX
            val dy = y - lastFlingY
            lastFlingX = x
            lastFlingY = y
            if (dx != 0 || dy != 0) {
                viewImpl.wheelEvent(
                    width / 2f, height / 2f,
                    width / 2f, height / 2f,
                    dx, dy
                )
            }
            postInvalidateOnAnimation()
        }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        // Only (re)allocate the scratch bitmap when it actually needs to grow.
        // System bar animations cause a flood of small size changes; allocating
        // a screen-sized RGBA8888 each time costs several MB of GC pressure and
        // visibly hurts scroll smoothness.
        if (!::contentBitmap.isInitialized || contentBitmap.width < w || contentBitmap.height < h) {
            val targetW = maxOf(w, if (::contentBitmap.isInitialized) contentBitmap.width else 0)
            val targetH = maxOf(h, if (::contentBitmap.isInitialized) contentBitmap.height else 0)
            contentBitmap = Bitmap.createBitmap(targetW, targetH, Bitmap.Config.ARGB_8888)
        }

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
    }

}
