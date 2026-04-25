package org.serenityos.ladybird

import android.view.View
import android.widget.EditText
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.UiController
import androidx.test.espresso.ViewAction
import androidx.test.espresso.action.ViewActions.pressImeActionButton
import androidx.test.espresso.action.ViewActions.replaceText
import androidx.test.espresso.assertion.ViewAssertions.matches
import androidx.test.espresso.matcher.ViewMatchers.isDisplayed
import androidx.test.espresso.matcher.ViewMatchers.withId
import androidx.test.ext.junit.rules.activityScenarioRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.hamcrest.Matcher
import org.hamcrest.Matchers.containsString
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

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

        // Wait up to 10 s using UiController (avoids raw Thread.sleep).
        onView(withId(R.id.urlEditText))
            .perform(waitUntilText(containsString("google.com"), timeoutMs = 10_000L))
    }

    // ---------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------

    /**
     * A [ViewAction] that loops the main thread in 500 ms bursts until the
     * text of the target [EditText] matches [matcher] or [timeoutMs] elapses.
     */
    private fun waitUntilText(matcher: Matcher<String>, timeoutMs: Long): ViewAction =
        object : ViewAction {
            override fun getConstraints(): Matcher<View> = isDisplayed()
            override fun getDescription() = "wait until EditText text matches [$matcher]"
            override fun perform(uiController: UiController, view: View) {
                val deadline = System.currentTimeMillis() + timeoutMs
                while (System.currentTimeMillis() < deadline) {
                    val text = (view as EditText).text.toString()
                    if (matcher.matches(text)) return
                    uiController.loopMainThreadForAtLeast(500)
                }
                val final = (view as EditText).text.toString()
                throw AssertionError(
                    "EditText text '$final' did not match [$matcher] within ${timeoutMs}ms"
                )
            }
        }
}
