/**
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.util.Log
import android.view.KeyEvent
import android.view.MenuItem
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.PopupMenu
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import org.serenityos.ladybird.databinding.ActivityMainBinding

class LadybirdActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var resourceDir: String
    private lateinit var view: WebView
    private lateinit var urlEditText: EditText
    private lateinit var settings: AppSettings
    private lateinit var bookmarks: BookmarksStore
    private lateinit var history: HistoryStore
    private var timerService = TimerExecutorService()
    private var nativeInitialized = false
    private var viewInitialized = false
    private var isLoading = false
    private var currentUrl: String = ""
    private var currentTitle: String = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        try {
            resourceDir = TransferAssets.transferAssets(this)
        } catch (exception: Exception) {
            Log.e("Ladybird", "Failed to prepare runtime assets", exception)
            finish()
            return
        }
        val userDir = applicationContext.getExternalFilesDir(null)!!.absolutePath
        initNativeCode(resourceDir, "Ladybird", timerService, userDir)
        nativeInitialized = true

        settings = AppSettings(this)
        bookmarks = BookmarksStore(this)
        history = HistoryStore(this)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)
        urlEditText = binding.urlEditText
        view = binding.webView

        view.onLoadStart = { url: String, _ ->
            setLoading(true)
            currentUrl = url
            if (!urlEditText.hasFocus())
                urlEditText.setText(url, TextView.BufferType.EDITABLE)
        }
        view.onLoadFinish = { url: String ->
            currentUrl = url
            setLoading(false)
            history.record(url, currentTitle.ifBlank { url })
        }
        view.onUrlChange = { url: String ->
            currentUrl = url
            if (!urlEditText.hasFocus())
                urlEditText.setText(url, TextView.BufferType.EDITABLE)
        }
        view.onTitleChange = { title: String ->
            currentTitle = title
        }
        view.onFindInPage = { current: Int, total: Int ->
            updateFindCounter(current, total)
        }
        view.onLinkHover = { url: String? ->
            if (!url.isNullOrEmpty())
                Log.d("Ladybird", "Hover: $url")
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

        setupFindBar()

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (binding.findInPageBar.visibility == View.VISIBLE) {
                    hideFindBar()
                } else {
                    view.goBack()
                }
            }
        })

        view.initialize(resourceDir)
        viewInitialized = true
        applySettingsToView()
        navigateToInput(intent.dataString ?: settings.homePage)
    }

    override fun onResume() {
        super.onResume()
        if (viewInitialized) applySettingsToView()
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

    private fun applySettingsToView() {
        view.setPreferredColorScheme(settings.colorScheme.nativeValue)
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
            return settings.homePage

        if (trimmedInput.startsWith("view-source:"))
            return trimmedInput

        val parsedUri = Uri.parse(trimmedInput)
        if (!parsedUri.scheme.isNullOrEmpty())
            return trimmedInput

        val looksLikeUrl = !trimmedInput.contains(WHITESPACE_REGEX) &&
            (trimmedInput.contains(".") ||
                trimmedInput.equals("localhost", ignoreCase = true) ||
                trimmedInput.startsWith("[") && trimmedInput.contains("]"))

        if (looksLikeUrl)
            return "https://$trimmedInput"

        return settings.searchEngine.urlFor(trimmedInput)
    }

    private fun setLoading(loading: Boolean) {
        isLoading = loading
        binding.loadingProgress.visibility = if (loading) View.VISIBLE else View.GONE
    }

    private fun hideKeyboard() {
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(urlEditText.windowToken, 0)
    }

    private fun setupFindBar() {
        binding.findInPageEdit.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                val q = s?.toString().orEmpty()
                if (q.isNotEmpty()) view.findInPage(q, caseSensitive = false)
                else updateFindCounter(0, 0)
            }
            override fun afterTextChanged(s: Editable?) {}
        })
        binding.findInPageEdit.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_SEARCH) {
                view.findNext(); true
            } else false
        }
        binding.findInPageNext.setOnClickListener { view.findNext() }
        binding.findInPagePrev.setOnClickListener { view.findPrevious() }
        binding.findInPageClose.setOnClickListener { hideFindBar() }
    }

    private fun showFindBar() {
        binding.findInPageBar.visibility = View.VISIBLE
        binding.findInPageEdit.requestFocus()
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(binding.findInPageEdit, InputMethodManager.SHOW_IMPLICIT)
    }

    private fun hideFindBar() {
        binding.findInPageEdit.setText("")
        binding.findInPageBar.visibility = View.GONE
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(binding.findInPageEdit.windowToken, 0)
        updateFindCounter(0, 0)
    }

    private fun updateFindCounter(current: Int, total: Int) {
        binding.findInPageCounter.text = if (total > 0)
            getString(R.string.find_in_page_counter, current, total)
        else if (binding.findInPageEdit.text.isNotEmpty())
            getString(R.string.find_in_page_no_matches)
        else ""
    }

    private fun showBrowserMenu(anchor: View) {
        PopupMenu(this, anchor).apply {
            menuInflater.inflate(R.menu.browser_menu, menu)
            // Mark currently active color scheme.
            when (settings.colorScheme) {
                ColorSchemePreference.Auto -> menu.findItem(R.id.menu_color_scheme_auto)?.isChecked = true
                ColorSchemePreference.Light -> menu.findItem(R.id.menu_color_scheme_light)?.isChecked = true
                ColorSchemePreference.Dark -> menu.findItem(R.id.menu_color_scheme_dark)?.isChecked = true
            }
            setOnMenuItemClickListener { item: MenuItem -> handleMenu(item) }
            show()
        }
    }

    private fun handleMenu(item: MenuItem): Boolean {
        return when (item.itemId) {
            R.id.menu_find_in_page -> { showFindBar(); true }
            R.id.menu_add_bookmark -> {
                if (currentUrl.isBlank()) return true
                val added = bookmarks.add(currentUrl, currentTitle.ifBlank { currentUrl })
                Toast.makeText(
                    this,
                    if (added) R.string.bookmark_added else R.string.bookmark_already_exists,
                    Toast.LENGTH_SHORT
                ).show()
                true
            }
            R.id.menu_bookmarks -> { showBookmarksDialog(); true }
            R.id.menu_history -> { showHistoryDialog(); true }
            R.id.menu_downloads -> {
                Toast.makeText(this, R.string.feature_not_available, Toast.LENGTH_SHORT).show(); true
            }
            R.id.menu_zoom_in -> { view.zoomIn(); true }
            R.id.menu_zoom_out -> { view.zoomOut(); true }
            R.id.menu_zoom_reset -> { view.zoomReset(); true }
            R.id.menu_color_scheme_auto -> { setColorScheme(ColorSchemePreference.Auto); true }
            R.id.menu_color_scheme_light -> { setColorScheme(ColorSchemePreference.Light); true }
            R.id.menu_color_scheme_dark -> { setColorScheme(ColorSchemePreference.Dark); true }
            R.id.menu_view_source -> {
                if (currentUrl.isNotBlank() && !currentUrl.startsWith("view-source:"))
                    view.loadURL("view-source:$currentUrl")
                true
            }
            R.id.menu_select_all -> { view.selectAllOnPage(); true }
            R.id.menu_share -> {
                if (currentUrl.isBlank()) return true
                val sendIntent = Intent(Intent.ACTION_SEND).apply {
                    type = "text/plain"
                    putExtra(Intent.EXTRA_TEXT, currentUrl)
                    putExtra(Intent.EXTRA_SUBJECT, currentTitle)
                }
                startActivity(Intent.createChooser(sendIntent, getString(R.string.menu_share)))
                true
            }
            R.id.menu_copy_url -> {
                val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("url", currentUrl))
                Toast.makeText(this, R.string.menu_copy_url, Toast.LENGTH_SHORT).show()
                true
            }
            R.id.menu_open_external -> {
                if (currentUrl.isNotBlank()) {
                    try {
                        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(currentUrl)))
                    } catch (_: Exception) {
                        Toast.makeText(this, R.string.feature_not_available, Toast.LENGTH_SHORT).show()
                    }
                }
                true
            }
            R.id.menu_settings -> {
                startActivity(Intent(this, SettingsActivity::class.java)); true
            }
            R.id.menu_about -> { showAboutDialog(); true }
            else -> false
        }
    }

    private fun setColorScheme(scheme: ColorSchemePreference) {
        settings.colorScheme = scheme
        view.setPreferredColorScheme(scheme.nativeValue)
    }

    private fun showBookmarksDialog() {
        val items = bookmarks.all()
        if (items.isEmpty()) {
            Toast.makeText(this, R.string.bookmarks_empty, Toast.LENGTH_SHORT).show()
            return
        }
        val titles = items.map { it.title.ifBlank { it.url } }.toTypedArray()
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.menu_bookmarks)
            .setItems(titles) { _, which -> navigateToInput(items[which].url) }
            .setNegativeButton(R.string.bookmark_remove) { _, _ -> showBookmarkRemoveDialog(items) }
            .setPositiveButton(R.string.dialog_ok, null)
            .show()
    }

    private fun showBookmarkRemoveDialog(items: List<Bookmark>) {
        val titles = items.map { it.title.ifBlank { it.url } }.toTypedArray()
        val checked = BooleanArray(items.size)
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.bookmark_remove)
            .setMultiChoiceItems(titles, checked) { _, which, isChecked -> checked[which] = isChecked }
            .setPositiveButton(R.string.dialog_ok) { _, _ ->
                items.forEachIndexed { i, b -> if (checked[i]) bookmarks.remove(b.url) }
            }
            .setNegativeButton(R.string.dialog_cancel, null)
            .show()
    }

    private fun showHistoryDialog() {
        val items = history.all()
        if (items.isEmpty()) {
            Toast.makeText(this, R.string.history_empty, Toast.LENGTH_SHORT).show()
            return
        }
        val titles = items.map { it.title.ifBlank { it.url } }.toTypedArray()
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.menu_history)
            .setItems(titles) { _, which -> navigateToInput(items[which].url) }
            .setNegativeButton(R.string.history_clear) { _, _ ->
                history.clear()
                Toast.makeText(this, R.string.history_cleared, Toast.LENGTH_SHORT).show()
            }
            .setPositiveButton(R.string.dialog_ok, null)
            .show()
    }

    private fun showAboutDialog() {
        val version = try {
            packageManager.getPackageInfo(packageName, 0).versionName ?: "dev"
        } catch (_: Exception) { "dev" }
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.about_ladybird_title)
            .setMessage(getString(R.string.about_ladybird_message, version))
            .setPositiveButton(R.string.dialog_ok, null)
            .setNeutralButton(R.string.about_visit_website) { _, _ ->
                try {
                    startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://ladybird.org/")))
                } catch (_: Exception) {}
            }
            .show()
    }

    private external fun initNativeCode(
        resourceDir: String, tag: String, timerService: TimerExecutorService, userDir: String
    )

    private external fun disposeNativeCode()
    private external fun execMainEventLoop()

    companion object {
        init {
            System.loadLibrary("Ladybird")
        }

        private val WHITESPACE_REGEX = Regex("\\s")
    }
}
