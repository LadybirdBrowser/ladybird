/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.PopupMenu
import android.widget.TextView
import android.widget.Toast
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import org.serenityos.ladybird.databinding.ActivityMainBinding

class LadybirdActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var resourceDir: String
    private lateinit var view: WebView
    private lateinit var urlEditText: EditText
    private var timerService = TimerExecutorService()
    private var nativeInitialized = false
    private var viewInitialized = false
    private var isLoading = false

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
            setLoading(true)
            urlEditText.setText(url, TextView.BufferType.EDITABLE)
        }
        view.onContentReady = {
            if (isLoading)
                setLoading(false)
        }
        view.onWebContentCrash = {
            setLoading(false)
            Snackbar.make(binding.root, R.string.browser_webcontent_crashed, Snackbar.LENGTH_LONG)
                .setAction(R.string.browser_reload) { view.reload() }
                .show()
        }
        urlEditText.setOnEditorActionListener { textView: TextView, actionId: Int, _: KeyEvent? ->
            when (actionId) {
                EditorInfo.IME_ACTION_GO, EditorInfo.IME_ACTION_SEARCH -> {
                    navigateToInput(textView.text.toString())
                    true
                }
                else -> false
            }
        }
        binding.backButton.setOnClickListener { view.goBack() }
        binding.forwardButton.setOnClickListener { view.goForward() }
        binding.reloadButton.setOnClickListener { view.reload() }
        binding.menuButton.setOnClickListener { showBrowserMenu(it) }
        view.initialize(resourceDir)
        viewInitialized = true
        navigateToInput(intent.dataString ?: DEFAULT_HOME_URL)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        intent.dataString?.let { navigateToInput(it) }
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

    private fun navigateToInput(input: String) {
        val url = normalizeUrlOrSearch(input)
        urlEditText.setText(url, TextView.BufferType.EDITABLE)
        urlEditText.clearFocus()
        hideKeyboard()
        setLoading(true)
        view.loadURL(url)
    }

    private fun normalizeUrlOrSearch(input: String): String {
        val trimmedInput = input.trim()
        if (trimmedInput.isEmpty())
            return DEFAULT_HOME_URL

        val parsedUri = Uri.parse(trimmedInput)
        if (!parsedUri.scheme.isNullOrEmpty())
            return trimmedInput

        val looksLikeUrl = !trimmedInput.contains(Regex("\\s")) &&
            (trimmedInput.contains(".") ||
                trimmedInput.equals("localhost", ignoreCase = true) ||
                trimmedInput.startsWith("[") && trimmedInput.contains("]"))

        if (looksLikeUrl)
            return "https://$trimmedInput"

        return "$DEFAULT_SEARCH_URL${Uri.encode(trimmedInput)}"
    }

    private fun setLoading(loading: Boolean) {
        isLoading = loading
        binding.loadingProgress.visibility = if (loading) View.VISIBLE else View.GONE
    }

    private fun hideKeyboard() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.hideSoftInputFromWindow(urlEditText.windowToken, 0)
    }

    private fun showBrowserMenu(anchor: View) {
        PopupMenu(this, anchor).apply {
            menuInflater.inflate(R.menu.browser_menu, menu)
            setOnMenuItemClickListener { menuItem ->
                when (menuItem.itemId) {
                    R.id.menu_about -> {
                        MaterialAlertDialogBuilder(this@LadybirdActivity)
                            .setTitle(R.string.menu_about)
                            .setMessage(R.string.about_ladybird_message)
                            .setPositiveButton(android.R.string.ok, null)
                            .show()
                        true
                    }
                    R.id.menu_settings, R.id.menu_bookmarks, R.id.menu_downloads -> {
                        Toast.makeText(this@LadybirdActivity, R.string.feature_not_available, Toast.LENGTH_SHORT).show()
                        true
                    }
                    else -> false
                }
            }
            show()
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

        private const val DEFAULT_HOME_URL = "https://ladybird.dev"
        private const val DEFAULT_SEARCH_URL = "https://www.google.com/search?q="
    }
}
