package org.serenityos.ladybird

import android.content.Context
import android.content.SharedPreferences

enum class SearchEngine(val displayName: String, val template: String) {
    Google("Google", "https://www.google.com/search?q=%s"),
    DuckDuckGo("DuckDuckGo", "https://duckduckgo.com/?q=%s"),
    Bing("Bing", "https://www.bing.com/search?q=%s"),
    Kagi("Kagi", "https://kagi.com/search?q=%s"),
    Brave("Brave", "https://search.brave.com/search?q=%s"),
    Ecosia("Ecosia", "https://www.ecosia.org/search?q=%s");

    fun urlFor(query: String): String = template.format(android.net.Uri.encode(query))

    companion object {
        fun from(name: String?): SearchEngine = entries.firstOrNull { it.name == name } ?: Google
    }
}

enum class ColorSchemePreference(val nativeValue: Int) {
    Auto(0), Light(1), Dark(2);

    companion object {
        fun from(name: String?): ColorSchemePreference = entries.firstOrNull { it.name == name } ?: Auto
    }
}

class AppSettings(context: Context) {
    private val prefs: SharedPreferences =
        context.getSharedPreferences("ladybird_prefs", Context.MODE_PRIVATE)

    var homePage: String
        get() = prefs.getString(KEY_HOME, DEFAULT_HOME) ?: DEFAULT_HOME
        set(value) = prefs.edit().putString(KEY_HOME, value.ifBlank { DEFAULT_HOME }).apply()

    var searchEngine: SearchEngine
        get() = SearchEngine.from(prefs.getString(KEY_SEARCH, SearchEngine.Google.name))
        set(value) = prefs.edit().putString(KEY_SEARCH, value.name).apply()

    var colorScheme: ColorSchemePreference
        get() = ColorSchemePreference.from(prefs.getString(KEY_COLOR, ColorSchemePreference.Auto.name))
        set(value) = prefs.edit().putString(KEY_COLOR, value.name).apply()

    var javascriptHelpersEnabled: Boolean
        get() = prefs.getBoolean(KEY_JS, true)
        set(value) = prefs.edit().putBoolean(KEY_JS, value).apply()

    fun resetToDefaults() {
        prefs.edit().clear().apply()
    }

    companion object {
        const val DEFAULT_HOME = "https://ladybird.org/"
        private const val KEY_HOME = "home_page"
        private const val KEY_SEARCH = "search_engine"
        private const val KEY_COLOR = "color_scheme"
        private const val KEY_JS = "js_helpers"
    }
}
