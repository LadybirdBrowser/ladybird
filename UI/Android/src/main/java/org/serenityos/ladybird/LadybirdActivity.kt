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
import android.os.SystemClock
import android.text.Editable
import android.text.TextWatcher
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.ImageView
import android.widget.PopupWindow
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.bottomsheet.BottomSheetDialog
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
    private var hasRenderedContent = false
    private var startupOverlayDismissed = false
    private var startupOverlayShownAt = 0L
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
        startStartupOverlayAnimation()

        view.onLoadStart = { url: String, _ ->
            Log.i("LadybirdLoad", "onLoadStart: $url")
            setLoading(true)
            currentUrl = url
            if (!urlEditText.hasFocus())
                urlEditText.setText(url, TextView.BufferType.EDITABLE)
        }
        view.onLoadFinish = { url: String ->
            Log.i("LadybirdLoad", "onLoadFinish: $url")
            currentUrl = url
            setLoading(false)
            history.record(url, currentTitle.ifBlank { url })
            // Nudge a single repaint once load settles; do NOT spam
            // setViewportGeometry on every load-finish (Google/SPAs trigger many
            // of these per click) — the engine already has the correct geometry
            // from onSizeChanged and per-rebind initialize_client.
            view.postInvalidateOnAnimation()
        }
        view.onUrlChange = { url: String ->
            Log.i("LadybirdLoad", "onUrlChange: $url")
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
            Log.i("LadybirdLoad", "onContentReady")
            hasRenderedContent = true
            if (isLoading)
                setLoading(false)
            hideStartupOverlayIfNeeded()
        }
        view.onWebContentCrash = {
            Log.e("LadybirdLoad", "onWebContentCrash")
            setLoading(false)
            applySettingsToView()
            view.syncViewport()
            // Suppress the spurious crash signal that fires once during initial
            // WebContent service bind, before any content has rendered.
            if (hasRenderedContent) {
                Snackbar.make(binding.root, R.string.browser_webcontent_crashed, Snackbar.LENGTH_LONG)
                    .setAction(R.string.browser_reload) {
                        if (currentUrl.isNotBlank()) view.loadURL(currentUrl)
                        else view.reload()
                    }
                    .show()
            }
        }
        view.onLongPress = { _, _ ->
            showPageContextMenu()
        }
        view.onSwipeRefresh = {
            if (currentUrl.isNotBlank()) view.loadURL(currentUrl) else view.reload()
        }

        urlEditText.setOnEditorActionListener { textView: TextView, actionId: Int, keyEvent: KeyEvent? ->
            val isImeSubmit = actionId == EditorInfo.IME_ACTION_GO || actionId == EditorInfo.IME_ACTION_SEARCH
            val isHardwareEnter = keyEvent?.keyCode == KeyEvent.KEYCODE_ENTER && keyEvent.action == KeyEvent.ACTION_DOWN
            if (isImeSubmit || isHardwareEnter) {
                navigateToInput(textView.text.toString())
                true
            } else {
                false
            }
        }
        binding.menuButton.setOnClickListener { showBrowserMenu() }
        binding.tabCountButton.setOnClickListener {
            Toast.makeText(this, R.string.browser_tabs_single, Toast.LENGTH_SHORT).show()
        }

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
        // Defer the initial navigation until the WebView has been laid out so
        // the WebContent viewport is sized correctly before the first render.
        val initialTarget = intent.dataString ?: settings.homePage
        view.post { navigateToInput(initialTarget) }
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
        view.setUserAgent(settings.userAgent)
        view.setNavigatorCompatibility(settings.navigatorCompatibility)
        view.setScriptingEnabled(settings.javascriptHelpersEnabled)
        view.setPinchZoomEnabled(settings.pinchZoomEnabled)
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

    private fun startStartupOverlayAnimation() {
        startupOverlayShownAt = SystemClock.elapsedRealtime()
        binding.startupOverlay.alpha = 1f
        binding.startupOverlay.visibility = View.VISIBLE
    }

    private fun hideStartupOverlayIfNeeded() {
        if (startupOverlayDismissed)
            return
        startupOverlayDismissed = true
        val elapsed = SystemClock.elapsedRealtime() - startupOverlayShownAt
        val remainingDelay = (450L - elapsed).coerceAtLeast(0L)
        binding.startupOverlay.postDelayed({
            binding.startupOverlay.animate()
                .alpha(0f)
                .setDuration(220)
                .withEndAction {
                    binding.startupOverlay.visibility = View.GONE
                }
                .start()
        }, remainingDelay)
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

    private fun showBrowserMenu() {
        val popupView = layoutInflater.inflate(R.layout.popup_overflow_menu, null)
        val popup = PopupWindow(
            popupView,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT,
            android.view.ViewGroup.LayoutParams.WRAP_CONTENT,
            true
        )
        popup.setBackgroundDrawable(android.graphics.drawable.ColorDrawable(android.graphics.Color.TRANSPARENT))
        popup.elevation = resources.displayMetrics.density * 8f
        popup.isOutsideTouchable = true

        // Top icon row
        popupView.findViewById<View>(R.id.menuForward).setOnClickListener {
            popup.dismiss(); view.goForward()
        }
        popupView.findViewById<View>(R.id.menuBookmarkAdd).setOnClickListener {
            popup.dismiss(); addCurrentBookmark()
        }
        popupView.findViewById<View>(R.id.menuHome).setOnClickListener {
            popup.dismiss(); navigateToInput(settings.homePage)
        }
        popupView.findViewById<View>(R.id.menuShare).setOnClickListener {
            popup.dismiss(); shareCurrent()
        }
        popupView.findViewById<View>(R.id.menuRefresh).setOnClickListener {
            popup.dismiss(); view.reload()
        }

        // List rows
        popupView.findViewById<View>(R.id.rowNewPage).setOnClickListener {
            popup.dismiss(); navigateToInput(settings.homePage)
        }
        popupView.findViewById<View>(R.id.rowHistory).setOnClickListener {
            popup.dismiss(); showHistorySheet()
        }
        popupView.findViewById<View>(R.id.rowBookmarks).setOnClickListener {
            popup.dismiss(); showBookmarksSheet()
        }
        popupView.findViewById<View>(R.id.rowFindInPage).setOnClickListener {
            popup.dismiss(); showFindBar()
        }
        popupView.findViewById<View>(R.id.rowViewSource).setOnClickListener {
            popup.dismiss(); openViewSource()
        }
        popupView.findViewById<View>(R.id.rowOpenExternal).setOnClickListener {
            popup.dismiss(); openCurrentInSystemBrowser()
        }
        popupView.findViewById<View>(R.id.rowClearCache).setOnClickListener {
            popup.dismiss()
            view.clearCache()
            view.collectGarbage()
            Toast.makeText(this, R.string.menu_clear_cache_done, Toast.LENGTH_SHORT).show()
        }
        popupView.findViewById<View>(R.id.rowSettings).setOnClickListener {
            popup.dismiss()
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        popupView.findViewById<View>(R.id.rowAbout).setOnClickListener {
            popup.dismiss(); showAboutDialog()
        }

        // Zoom controls
        val zoomLabel = popupView.findViewById<TextView>(R.id.zoomLabel)
        fun updateZoom() {
            val pct = (view.zoomLevel() * 100).toInt()
            zoomLabel.text = "$pct%"
        }
        updateZoom()
        popupView.findViewById<View>(R.id.zoomInButton).setOnClickListener {
            view.zoomIn(); updateZoom()
        }
        popupView.findViewById<View>(R.id.zoomOutButton).setOnClickListener {
            view.zoomOut(); updateZoom()
        }
        zoomLabel.setOnClickListener { view.zoomReset(); updateZoom() }

        // Anchor at top-right under the 3-dot button, like Chromium
        popup.showAsDropDown(binding.menuButton, 0, 0, android.view.Gravity.END)
    }

    private fun addCurrentBookmark() {
        if (currentUrl.isBlank()) return
        val added = bookmarks.add(currentUrl, currentTitle.ifBlank { currentUrl })
        Toast.makeText(
            this,
            if (added) R.string.bookmark_added else R.string.bookmark_already_exists,
            Toast.LENGTH_SHORT
        ).show()
    }

    private fun shareCurrent() {
        if (currentUrl.isBlank()) return
        val sendIntent = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_TEXT, currentUrl)
            putExtra(Intent.EXTRA_SUBJECT, currentTitle)
        }
        startActivity(Intent.createChooser(sendIntent, getString(R.string.menu_share)))
    }

    private fun copyCurrentUrl() {
        if (currentUrl.isBlank()) return
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("url", currentUrl))
        Toast.makeText(this, R.string.menu_copy_url, Toast.LENGTH_SHORT).show()
    }

    private fun openViewSource() {
        if (currentUrl.isNotBlank() && !currentUrl.startsWith("view-source:"))
            view.loadURL("view-source:$currentUrl")
    }

    private fun openCurrentInSystemBrowser() {
        if (currentUrl.isBlank()) return
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(currentUrl)))
        } catch (_: Exception) {
            Toast.makeText(this, R.string.feature_not_available, Toast.LENGTH_SHORT).show()
        }
    }

    private fun setColorScheme(scheme: ColorSchemePreference) {
        settings.colorScheme = scheme
        view.setPreferredColorScheme(scheme.nativeValue)
    }

    private fun showBookmarksSheet() {
        val initial = bookmarks.all().map { UrlRow(it.url, it.title.ifBlank { it.url }) }
        showUrlListSheet(
            iconRes = R.drawable.ic_bookmark,
            titleRes = R.string.bookmarks_title,
            emptyRes = R.string.bookmarks_empty,
            subtitleFormat = R.string.bookmarks_subtitle,
            actionLabel = if (initial.isNotEmpty()) R.string.dialog_done else 0,
            initial = initial,
            onActivate = { row -> navigateToInput(row.url) },
            onDelete = { row ->
                bookmarks.remove(row.url)
                Toast.makeText(this, R.string.bookmark_removed, Toast.LENGTH_SHORT).show()
            },
            onAction = null
        )
    }

    private fun showHistorySheet() {
        val initial = history.all().map { UrlRow(it.url, it.title.ifBlank { it.url }) }
        showUrlListSheet(
            iconRes = R.drawable.ic_history,
            titleRes = R.string.history_title,
            emptyRes = R.string.history_empty,
            subtitleFormat = R.string.history_subtitle,
            actionLabel = if (initial.isNotEmpty()) R.string.history_clear else 0,
            initial = initial,
            onActivate = { row -> navigateToInput(row.url) },
            onDelete = { row -> history.remove(row.url) },
            onAction = { dismiss ->
                MaterialAlertDialogBuilder(this)
                    .setTitle(R.string.history_clear)
                    .setPositiveButton(R.string.dialog_clear) { _, _ ->
                        history.clear()
                        dismiss()
                        Toast.makeText(this, R.string.history_cleared, Toast.LENGTH_SHORT).show()
                    }
                    .setNegativeButton(R.string.dialog_cancel, null)
                    .show()
            }
        )
    }

    private fun showUrlListSheet(
        iconRes: Int,
        titleRes: Int,
        emptyRes: Int,
        subtitleFormat: Int,
        actionLabel: Int,
        initial: List<UrlRow>,
        onActivate: (UrlRow) -> Unit,
        onDelete: ((UrlRow) -> Unit)?,
        onAction: ((() -> Unit) -> Unit)?
    ) {
        val dialog = BottomSheetDialog(this)
        val sheet = layoutInflater.inflate(R.layout.sheet_url_list, null)
        dialog.setContentView(sheet)

        sheet.findViewById<ImageView>(R.id.sheetIcon).setImageResource(iconRes)
        sheet.findViewById<TextView>(R.id.sheetTitle).setText(titleRes)
        val subtitle = sheet.findViewById<TextView>(R.id.sheetSubtitle)
        subtitle.text = getString(subtitleFormat, initial.size)

        val list = sheet.findViewById<RecyclerView>(R.id.sheetList)
        list.layoutManager = LinearLayoutManager(this)
        val emptyState = sheet.findViewById<View>(R.id.sheetEmptyState)
        sheet.findViewById<TextView>(R.id.sheetEmptyText).setText(emptyRes)

        val rows = initial.toMutableList()
        lateinit var adapter: UrlListAdapter
        val deleteCallback = onDelete

        fun refreshState() {
            emptyState.visibility = if (rows.isEmpty()) View.VISIBLE else View.GONE
            list.visibility = if (rows.isEmpty()) View.GONE else View.VISIBLE
            subtitle.text = getString(subtitleFormat, rows.size)
        }

        adapter = UrlListAdapter(
            rows,
            onClick = { row -> dialog.dismiss(); onActivate(row) },
            onDelete = if (deleteCallback != null) { row ->
                val idx = rows.indexOf(row)
                if (idx >= 0) {
                    adapter.removeAt(idx)
                    deleteCallback(row)
                    refreshState()
                }
            } else null
        )
        list.adapter = adapter
        refreshState()

        val actionButton = sheet.findViewById<com.google.android.material.button.MaterialButton>(R.id.sheetActionButton)
        if (actionLabel != 0) {
            actionButton.visibility = View.VISIBLE
            actionButton.setText(actionLabel)
            actionButton.setOnClickListener {
                if (onAction != null) onAction { dialog.dismiss() }
                else dialog.dismiss()
            }
        } else {
            actionButton.visibility = View.GONE
        }

        dialog.show()
    }

    private fun showPageContextMenu() {
        val items = mutableListOf<Pair<Int, () -> Unit>>()
        if (currentUrl.isNotBlank()) {
            items += R.string.context_copy_url to { copyCurrentUrl() }
            items += R.string.menu_share to { shareCurrent() }
            items += R.string.menu_reload to { view.reload() }
            items += R.string.context_view_source to { openViewSource() }
            items += R.string.menu_find to { showFindBar() }
        }
        items += R.string.menu_bookmark to { addCurrentBookmark() }
        items += R.string.menu_select_all to { view.selectAllOnPage() }

        val labels = items.map { getString(it.first) }.toTypedArray()
        MaterialAlertDialogBuilder(this)
            .setTitle(if (currentTitle.isNotBlank()) currentTitle else getString(R.string.app_name))
            .setItems(labels) { _, idx -> items[idx].second() }
            .setNegativeButton(R.string.dialog_cancel, null)
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
