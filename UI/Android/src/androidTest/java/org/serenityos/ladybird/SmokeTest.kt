package org.serenityos.ladybird

import androidx.test.ext.junit.rules.activityScenarioRule
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.action.ViewActions.pressImeActionButton
import androidx.test.espresso.action.ViewActions.replaceText
import androidx.test.espresso.assertion.ViewAssertions.matches
import androidx.test.espresso.matcher.ViewMatchers.isDisplayed
import androidx.test.espresso.matcher.ViewMatchers.withId
import androidx.test.espresso.matcher.ViewMatchers.withText
import org.hamcrest.Matchers.containsString

import org.junit.Test
import org.junit.runner.RunWith

import org.junit.Assert.*
import org.junit.Rule

/**
 * Instrumented test, which will execute on an Android device.
 *
 * See [testing documentation](http://d.android.com/tools/testing).
 */
@RunWith(AndroidJUnit4::class)
class SmokeTest {

    @get:Rule
    var activityScenarioRule = activityScenarioRule<LadybirdActivity>()

    @Test
    fun useAppContext() {
        // Context of the app under test.
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        assertEquals("org.serenityos.ladybird", appContext.packageName)
    }

    @Test
    fun loadWebView() {
        // We can actually load a web view, and it is visible
        onView(withId(R.id.web_view)).check(matches(isDisplayed()))
    }

    @Test
    fun urlBarNavigatesToGoogle() {
        // Type URL and press Go; the onLoadStart callback updates the URL bar
        // when the native browser starts the navigation, proving the full
        // browser stack (native library, RequestServer, networking) is alive.
        onView(withId(R.id.urlEditText))
            .perform(replaceText("https://www.google.com"), pressImeActionButton())

        // Allow up to ~10 s for the native engine to start loading and invoke
        // the onLoadStart callback which writes the URL back to the URL bar.
        val deadline = System.currentTimeMillis() + 10_000L
        var passed = false
        while (System.currentTimeMillis() < deadline) {
            try {
                onView(withId(R.id.urlEditText))
                    .check(matches(withText(containsString("google.com"))))
                passed = true
                break
            } catch (_: AssertionError) {
                Thread.sleep(500)
            }
        }
        assertTrue("URL bar did not update to google.com within timeout", passed)
    }
}
