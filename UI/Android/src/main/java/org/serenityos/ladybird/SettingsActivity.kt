package org.serenityos.ladybird

import android.os.Bundle
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * Settings apply instantly as the user changes them, matching the behaviour of
 * Chromium-based browsers (Vanadium/Chrome) which have no explicit Save action.
 */
class SettingsActivity : AppCompatActivity() {

    private lateinit var settings: AppSettings
    private lateinit var binding: org.serenityos.ladybird.databinding.ActivitySettingsBinding

    // Guards spinner/radio listeners from firing while we apply the initial state.
    private var bindingInitialState = true

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = org.serenityos.ladybird.databinding.ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        settings = AppSettings(this)

        setSupportActionBar(binding.settingsToolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        binding.settingsToolbar.setNavigationOnClickListener { finish() }

        bindState()
        wireListeners()
    }

    private fun bindState() {
        bindingInitialState = true

        val engines = SearchEngine.entries
        binding.searchEngineSpinner.adapter = ArrayAdapter(
            this,
            R.layout.spinner_item,
            engines.map { it.displayName }
        ).also { it.setDropDownViewResource(R.layout.spinner_dropdown_item) }
        binding.searchEngineSpinner.setSelection(engines.indexOf(settings.searchEngine))

        val agents = UserAgentPreset.entries
        binding.userAgentSpinner.adapter = ArrayAdapter(
            this,
            R.layout.spinner_item,
            agents.map { it.displayName }
        ).also { it.setDropDownViewResource(R.layout.spinner_dropdown_item) }
        binding.userAgentSpinner.setSelection(agents.indexOf(settings.userAgent))

        val compats = NavigatorCompatibility.entries
        binding.navCompatSpinner.adapter = ArrayAdapter(
            this,
            R.layout.spinner_item,
            compats.map { it.displayName }
        ).also { it.setDropDownViewResource(R.layout.spinner_dropdown_item) }
        binding.navCompatSpinner.setSelection(compats.indexOf(settings.navigatorCompatibility))

        binding.homePageEdit.setText(settings.homePage)

        binding.colorSchemeGroup.check(
            when (settings.colorScheme) {
                ColorSchemePreference.Light -> R.id.colorSchemeLight
                ColorSchemePreference.Dark -> R.id.colorSchemeDark
                ColorSchemePreference.Auto -> R.id.colorSchemeAuto
            }
        )

        binding.jsSwitch.isChecked = settings.javascriptHelpersEnabled
        binding.pinchSwitch.isChecked = settings.pinchZoomEnabled

        val version = try {
            packageManager.getPackageInfo(packageName, 0).versionName ?: "dev"
        } catch (_: Exception) { "dev" }
        binding.aboutVersion.text = getString(R.string.settings_about_version, version)

        bindingInitialState = false
    }

    private fun wireListeners() {
        val engines = SearchEngine.entries
        binding.searchEngineSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                if (!bindingInitialState) settings.searchEngine = engines[position]
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        val agents = UserAgentPreset.entries
        binding.userAgentSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                if (!bindingInitialState) settings.userAgent = agents[position]
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        val compats = NavigatorCompatibility.entries
        binding.navCompatSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                if (!bindingInitialState) settings.navigatorCompatibility = compats[position]
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        // Persist the home page when the field loses focus (instant apply).
        binding.homePageEdit.setOnFocusChangeListener { _, hasFocus ->
            if (!hasFocus) persistHomePage()
        }

        binding.colorSchemeGroup.setOnCheckedChangeListener { _, checkedId ->
            if (bindingInitialState) return@setOnCheckedChangeListener
            settings.colorScheme = when (checkedId) {
                R.id.colorSchemeLight -> ColorSchemePreference.Light
                R.id.colorSchemeDark -> ColorSchemePreference.Dark
                else -> ColorSchemePreference.Auto
            }
        }

        binding.jsSwitch.setOnCheckedChangeListener { _, isChecked ->
            if (!bindingInitialState) settings.javascriptHelpersEnabled = isChecked
        }
        binding.pinchSwitch.setOnCheckedChangeListener { _, isChecked ->
            if (!bindingInitialState) settings.pinchZoomEnabled = isChecked
        }

        binding.clearDataRow.setOnClickListener {
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.settings_clear_data)
                .setMessage(R.string.settings_clear_data_confirm)
                .setPositiveButton(R.string.dialog_clear) { _, _ ->
                    HistoryStore(this).clear()
                    BookmarksStore(this).clear()
                    Toast.makeText(this, R.string.settings_clear_data_done, Toast.LENGTH_SHORT).show()
                }
                .setNegativeButton(R.string.dialog_cancel, null)
                .show()
        }

        binding.resetRow.setOnClickListener {
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.settings_reset)
                .setMessage(R.string.settings_reset_summary)
                .setPositiveButton(R.string.dialog_reset) { _, _ ->
                    settings.resetToDefaults()
                    bindState()
                }
                .setNegativeButton(R.string.dialog_cancel, null)
                .show()
        }
    }

    private fun persistHomePage() {
        val value = binding.homePageEdit.text.toString().trim()
        if (value != settings.homePage) settings.homePage = value
    }

    override fun onPause() {
        // Make sure an in-progress edit is committed even without losing focus.
        persistHomePage()
        super.onPause()
    }
}
