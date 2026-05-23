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

/**
 * Identifies a User-Agent preset that the engine can spoof. The native side
 * understands these identifiers through the `spoof-user-agent` debug request
 * with the full UA string as argument; the strings below mirror the
 * `WebView::user_agents` table in libwebview.
 *
 * `Default` keeps the engine-provided UA (which advertises Ladybird and is
 * frequently misclassified by anti-bot services such as reCAPTCHA).
 */
enum class UserAgentPreset(val displayName: String, val uaString: String?) {
    Default("Default (Ladybird)", null),
    ChromeAndroid(
        "Chrome (Android)",
        "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/127.0.0.0 Mobile Safari/537.36"
    ),
    ChromeDesktop(
        "Chrome (Desktop)",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/127.0.0.0 Safari/537.36"
    ),
    FirefoxAndroid(
        "Firefox (Android)",
        "Mozilla/5.0 (Android 13; Mobile; rv:129.0) Gecko/129.0 Firefox/129.0"
    ),
    SafariIOS(
        "Safari (iOS)",
        "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5_1 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1"
    );

    companion object {
        fun from(name: String?): UserAgentPreset = entries.firstOrNull { it.name == name } ?: ChromeDesktop
    }
}

enum class NavigatorCompatibility(val displayName: String, val nativeName: String) {
    Chrome("Chrome", "chrome"),
    Gecko("Gecko (Firefox)", "gecko"),
    WebKit("WebKit (Safari)", "webkit");

    companion object {
        fun from(name: String?): NavigatorCompatibility = entries.firstOrNull { it.name == name } ?: Chrome
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

    /**
      * Default to desktop Chrome for now. Google mobile search still tends to
      * route Ladybird-like clients through anti-bot flows, while the desktop UA
      * produces simpler layouts and has been a better compatibility baseline on
      * Android during bring-up. Users can still switch to mobile Chrome in
      * Settings once site compatibility improves.
     */
    var userAgent: UserAgentPreset
          get() = UserAgentPreset.from(prefs.getString(KEY_UA, UserAgentPreset.ChromeDesktop.name))
        set(value) = prefs.edit().putString(KEY_UA, value.name).apply()

    var navigatorCompatibility: NavigatorCompatibility
        get() = NavigatorCompatibility.from(prefs.getString(KEY_NAV_COMPAT, NavigatorCompatibility.Chrome.name))
        set(value) = prefs.edit().putString(KEY_NAV_COMPAT, value.name).apply()

    var pinchZoomEnabled: Boolean
        get() = prefs.getBoolean(KEY_PINCH, true)
        set(value) = prefs.edit().putBoolean(KEY_PINCH, value).apply()

    fun resetToDefaults() {
        prefs.edit().clear().apply()
    }

    companion object {
        const val DEFAULT_HOME = "https://ladybird.org/"
        private const val KEY_HOME = "home_page"
        private const val KEY_SEARCH = "search_engine"
        private const val KEY_COLOR = "color_scheme"
        private const val KEY_JS = "js_helpers"
        private const val KEY_UA = "user_agent_preset"
        private const val KEY_NAV_COMPAT = "navigator_compat"
        private const val KEY_PINCH = "pinch_zoom_enabled"
    }
}
