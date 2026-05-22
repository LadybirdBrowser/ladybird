package org.serenityos.ladybird

import android.os.Bundle
import android.widget.ArrayAdapter
import androidx.appcompat.app.AppCompatActivity

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
        val adapter = ArrayAdapter(
            this,
            R.layout.spinner_item,
            engines.map { it.displayName }
        ).also { it.setDropDownViewResource(R.layout.spinner_dropdown_item) }
        binding.searchEngineSpinner.adapter = adapter
        binding.searchEngineSpinner.setSelection(engines.indexOf(settings.searchEngine))

        binding.homePageEdit.setText(settings.homePage)

        binding.colorSchemeGroup.check(
            when (settings.colorScheme) {
                ColorSchemePreference.Light -> R.id.colorSchemeLight
                ColorSchemePreference.Dark -> R.id.colorSchemeDark
                ColorSchemePreference.Auto -> R.id.colorSchemeAuto
            }
        )

        binding.jsSwitch.isChecked = settings.javascriptHelpersEnabled

        binding.saveButton.setOnClickListener {
            settings.homePage = binding.homePageEdit.text.toString().trim()
            settings.searchEngine = engines[binding.searchEngineSpinner.selectedItemPosition]
            settings.colorScheme = when (binding.colorSchemeGroup.checkedRadioButtonId) {
                R.id.colorSchemeLight -> ColorSchemePreference.Light
                R.id.colorSchemeDark -> ColorSchemePreference.Dark
                else -> ColorSchemePreference.Auto
            }
            settings.javascriptHelpersEnabled = binding.jsSwitch.isChecked
            finish()
        }

        binding.resetButton.setOnClickListener {
            settings.resetToDefaults()
            recreate()
        }
    }
}
