/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.view.KeyEvent
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.TextView
import org.serenityos.ladybird.databinding.ActivityMainBinding

class LadybirdActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var resourceDir: String
    private lateinit var view: WebView
    private lateinit var urlEditText: EditText
    private var timerService = TimerExecutorService()
    private var nativeInitialized = false
    private var viewInitialized = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        try {
            resourceDir = TransferAssets.transferAssets(this)
        } catch (exception: Exception) {
            Log.e("Ladybird", "Failed to prepare runtime assets", exception)
            finish()
            return
        }
        val userDir = applicationContext.getExternalFilesDir(null)!!.absolutePath;
        initNativeCode(resourceDir, "Ladybird", timerService, userDir)
        nativeInitialized = true

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)
        urlEditText = binding.urlEditText
        view = binding.webView
        view.onLoadStart = { url: String, _ ->
            urlEditText.setText(url, TextView.BufferType.EDITABLE)
        }
        urlEditText.setOnEditorActionListener { textView: TextView, actionId: Int, _: KeyEvent? ->
            when (actionId) {
                EditorInfo.IME_ACTION_GO, EditorInfo.IME_ACTION_SEARCH -> view.loadURL(textView.text.toString())
            }
            false
        }
        view.initialize(resourceDir)
        viewInitialized = true
        view.loadURL(intent.dataString ?: "https://ladybird.dev")
    }

    override fun onStart() {
        super.onStart()
    }

    override fun onDestroy() {
        if (viewInitialized)
            view.dispose()
        if (nativeInitialized)
            disposeNativeCode()
        super.onDestroy()
    }

    private fun scheduleEventLoop() {
        mainExecutor.execute {
            execMainEventLoop()
        }
    }

    private external fun initNativeCode(
        resourceDir: String, tag: String, timerService: TimerExecutorService, userDir: String
    )

    private external fun disposeNativeCode()
    private external fun execMainEventLoop()

    companion object {
        // Used to load the 'ladybird' library on application startup.
        init {
            System.loadLibrary("Ladybird")
        }
    }
}
