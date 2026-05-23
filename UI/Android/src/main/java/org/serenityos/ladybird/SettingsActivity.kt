package org.serenityos.ladybird

import android.os.Bundle
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder

class SettingsActivity : AppCompatActivity() {

    private lateinit var settings: AppSettings
    private lateinit var binding: org.serenityos.ladybird.databinding.ActivitySettingsBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = org.serenityos.ladybird.databinding.ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        settings = AppSettings(this)

        setSupportActionBar(binding.settingsToolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        binding.settingsToolbar.setNavigationOnClickListener { finish() }

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

        binding.saveButton.setOnClickListener {
            settings.homePage = binding.homePageEdit.text.toString().trim()
            settings.searchEngine = engines[binding.searchEngineSpinner.selectedItemPosition]
            settings.userAgent = agents[binding.userAgentSpinner.selectedItemPosition]
            settings.navigatorCompatibility = compats[binding.navCompatSpinner.selectedItemPosition]
            settings.colorScheme = when (binding.colorSchemeGroup.checkedRadioButtonId) {
                R.id.colorSchemeLight -> ColorSchemePreference.Light
                R.id.colorSchemeDark -> ColorSchemePreference.Dark
                else -> ColorSchemePreference.Auto
            }
            settings.javascriptHelpersEnabled = binding.jsSwitch.isChecked
            settings.pinchZoomEnabled = binding.pinchSwitch.isChecked
            finish()
        }

        binding.resetButton.setOnClickListener {
            settings.resetToDefaults()
            recreate()
        }
    }
}
