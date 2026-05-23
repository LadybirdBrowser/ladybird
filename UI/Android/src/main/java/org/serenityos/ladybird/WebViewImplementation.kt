/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.graphics.Bitmap
import android.util.Log
import android.view.MotionEvent
import android.view.View

/**
 * Wrapper around WebView::ViewImplementation for use by Kotlin
 */
class WebViewImplementation(private val view: WebView) {
    // Instance Pointer to native object, very unsafe :)
    private var nativeInstance: Long = 0
    private lateinit var resourceDir: String
    private var connection: ServiceConnection? = null

    fun initialize(resourceDir: String) {
        this.resourceDir = resourceDir
        Log.i("WebContentView", "Creating native WebView implementation")
        nativeInstance = nativeObjectInit()
        Log.i("WebContentView", "Native WebView implementation ready")
    }

    fun dispose() {
        connection?.let {
            view.context.unbindService(it)
            connection = null
        }
        if (nativeInstance == 0L)
            return
        nativeObjectDispose(nativeInstance)
        nativeInstance = 0
    }

    fun loadURL(url: String) {
        if (nativeInstance == 0L)
            return
        nativeLoadURL(nativeInstance, url)
    }

    fun reload() {
        if (nativeInstance == 0L)
            return
        nativeReload(nativeInstance)
    }

    fun goBack() {
        if (nativeInstance == 0L)
            return
        nativeTraverseHistory(nativeInstance, -1)
    }

    fun goForward() {
        if (nativeInstance == 0L)
            return
        nativeTraverseHistory(nativeInstance, 1)
    }

    fun findInPage(query: String, caseSensitive: Boolean = false) {
        if (nativeInstance == 0L)
            return
        nativeFindInPage(nativeInstance, query, caseSensitive)
    }

    fun findNext() {
        if (nativeInstance == 0L)
            return
        nativeFindNext(nativeInstance)
    }

    fun findPrevious() {
        if (nativeInstance == 0L)
            return
        nativeFindPrevious(nativeInstance)
    }

    fun zoomIn() {
        if (nativeInstance == 0L)
            return
        nativeZoomIn(nativeInstance)
    }

    fun zoomOut() {
        if (nativeInstance == 0L)
            return
        nativeZoomOut(nativeInstance)
    }

    fun zoomReset() {
        if (nativeInstance == 0L)
            return
        nativeZoomReset(nativeInstance)
    }

    fun zoomLevel(): Double {
        if (nativeInstance == 0L)
            return 1.0
        return nativeZoomLevel(nativeInstance)
    }

    fun setPreferredColorScheme(scheme: Int) {
        if (nativeInstance == 0L)
            return
        nativeSetPreferredColorScheme(nativeInstance, scheme)
    }

    fun runJavascript(js: String) {
        if (nativeInstance == 0L)
            return
        nativeRunJavascript(nativeInstance, js)
    }

    fun selectAllOnPage() {
        if (nativeInstance == 0L)
            return
        nativeSelectAll(nativeInstance)
    }

    fun debugRequest(request: String, argument: String? = null) {
        if (nativeInstance == 0L)
            return
        nativeDebugRequest(nativeInstance, request, argument)
    }

    fun drawIntoBitmap(bitmap: Bitmap) {
        if (nativeInstance == 0L)
            return
        nativeDrawIntoBitmap(nativeInstance, bitmap)
    }

    fun setViewportGeometry(w: Int, h: Int) {
        if (nativeInstance == 0L)
            return
        nativeSetViewportGeometry(nativeInstance, w, h)
    }

    fun setDevicePixelRatio(ratio: Float) {
        if (nativeInstance == 0L)
            return
        nativeSetDevicePixelRatio(nativeInstance, ratio)
    }

    fun mouseEvent(eventType: Int, x: Float, y: Float, rawX: Float, rawY: Float) {
        if (nativeInstance == 0L)
            return
        nativeMouseEvent(nativeInstance, eventType, x, y, rawX, rawY)
    }

    fun wheelEvent(x: Float, y: Float, rawX: Float, rawY: Float, wheelDeltaX: Int, wheelDeltaY: Int) {
        if (nativeInstance == 0L)
            return
        nativeWheelEvent(nativeInstance, x, y, rawX, rawY, wheelDeltaX, wheelDeltaY)
    }

    // Functions called from native code
    fun bindWebContentService(ipcFd: Int) {
        Log.i("WebContentView", "Binding WebContent service with IPC fd $ipcFd")
        val connector = LadybirdServiceConnection(ipcFd, resourceDir)
        connector.onDisconnect = {
            Log.e("WebContentView", "WebContent Died! :(")
            view.onWebContentCrash()
        }
        val bound = view.context.bindService(
            Intent(view.context, WebContentService::class.java),
            connector,
            Context.BIND_AUTO_CREATE
        )
        Log.i("WebContentView", "bindService(WebContentService) returned $bound")
        if (bound)
            connection = connector
        else
            view.onWebContentCrash()
    }

    fun invalidateLayout() {
        view.requestLayout()
        view.invalidate()
        view.onContentReady()
    }

    fun onLoadStart(url: String, isRedirect: Boolean) {
        view.onLoadStart(url, isRedirect)
    }

    fun onLoadFinish(url: String) {
        view.onLoadFinish(url)
    }

    fun onTitleChange(title: String) {
        view.onTitleChange(title)
    }

    fun onUrlChange(url: String) {
        view.onUrlChange(url)
    }

    fun onFindInPage(currentMatch: Int, totalMatches: Int) {
        view.onFindInPage(currentMatch, totalMatches)
    }

    fun onLinkHover(url: String?) {
        view.onLinkHover(url)
    }

    // Functions implemented in native code
    private external fun nativeObjectInit(): Long
    private external fun nativeObjectDispose(instance: Long)

    private external fun nativeDrawIntoBitmap(instance: Long, bitmap: Bitmap)
    private external fun nativeSetViewportGeometry(instance: Long, w: Int, h: Int)
    private external fun nativeSetDevicePixelRatio(instance: Long, ratio: Float)
    private external fun nativeLoadURL(instance: Long, url: String)
    private external fun nativeReload(instance: Long)
    private external fun nativeTraverseHistory(instance: Long, delta: Int)
    private external fun nativeMouseEvent(instance: Long, eventType: Int, x: Float, y: Float, rawX: Float, rawY: Float)
    private external fun nativeWheelEvent(instance: Long, x: Float, y: Float, rawX: Float, rawY: Float, wheelDeltaX: Int, wheelDeltaY: Int)
    private external fun nativeFindInPage(instance: Long, query: String, caseSensitive: Boolean)
    private external fun nativeFindNext(instance: Long)
    private external fun nativeFindPrevious(instance: Long)
    private external fun nativeZoomIn(instance: Long)
    private external fun nativeZoomOut(instance: Long)
    private external fun nativeZoomReset(instance: Long)
    private external fun nativeZoomLevel(instance: Long): Double
    private external fun nativeSetPreferredColorScheme(instance: Long, scheme: Int)
    private external fun nativeRunJavascript(instance: Long, js: String)
    private external fun nativeSelectAll(instance: Long)
    private external fun nativeDebugRequest(instance: Long, request: String, argument: String?)

    companion object {
        /*
         * We use a static class initializer to allow the native code to cache some
         * field offsets. This native function looks up and caches interesting
         * class/field/method IDs. Throws on failure.
         */
        private external fun nativeClassInit()

        init {
            nativeClassInit()
        }
    }
};
