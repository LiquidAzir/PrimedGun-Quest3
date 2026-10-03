// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.activities

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AlertDialog
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.features.settings.ui.MenuTag
import org.dolphinemu.dolphinemu.features.settings.ui.SettingsActivity
import org.dolphinemu.dolphinemu.features.settings.model.QuestVrSettings
import org.dolphinemu.dolphinemu.features.settings.model.Settings
import org.dolphinemu.dolphinemu.model.GameFile
import org.dolphinemu.dolphinemu.ui.main.MainActivity
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import java.io.File

/** A 2D setup panel; only EmulationActivity requests an immersive OpenXR session. */
class QuestLauncherActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private lateinit var launchButton: Button
    private lateinit var chooseButton: Button
    private var selectedPath: String? = null
    private var selectionGeneration = 0

    private val preferences by lazy { getSharedPreferences("quest_launcher", MODE_PRIVATE) }
    private val gamesDirectory: File
        get() = File(DirectoryInitialization.getUserDirectory(), "Games")

    private val chooseRom = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            try {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            } catch (_: SecurityException) {
                // Some providers offer only a temporary grant. Validate access again at launch.
            }
            selectRom(uri.toString())
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val spacing = (24 * resources.displayMetrics.density).toInt()
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(spacing, spacing, spacing, spacing)
        }
        setContentView(ScrollView(this).apply { addView(content) })
        content.addView(TextView(this).apply {
            setText(R.string.quest_launcher_title)
            textSize = 28f
        })
        content.addView(TextView(this).apply {
            setText(R.string.quest_launcher_description)
            textSize = 18f
            setPadding(0, spacing / 2, 0, spacing)
        })
        status = TextView(this).apply {
            setText(R.string.quest_preparing)
            textSize = 16f
            setTextIsSelectable(true)
            setPadding(0, 0, 0, spacing / 2)
        }
        content.addView(status)
        launchButton = content.addButton(R.string.quest_launch_vr) {
            selectedPath?.let { selectRom(it, launch = true) }
        }.apply { isEnabled = false }
        chooseButton = content.addButton(R.string.quest_choose_rom) {
            try {
                chooseRom.launch(arrayOf("*/*"))
            } catch (_: ActivityNotFoundException) {
                status.text = getString(R.string.quest_no_picker, gamesDirectory.absolutePath)
            }
        }.apply { isEnabled = false }
        content.addButton(R.string.quest_scan_roms) { findRom(ignorePrevious = true) }
        content.addButton(R.string.quest_graphics_presets) {
            if (DirectoryInitialization.areDolphinDirectoriesReady()) showGraphicsPresets()
        }
        content.addButton(R.string.quest_graphics_settings) {
            if (DirectoryInitialization.areDolphinDirectoriesReady())
                SettingsActivity.launch(this, MenuTag.ENHANCEMENTS)
        }
        content.addButton(R.string.quest_library_settings) {
            if (DirectoryInitialization.areDolphinDirectoriesReady())
                startActivity(Intent(this, MainActivity::class.java))
        }
        content.addView(TextView(this).apply {
            setText(R.string.quest_controls_help)
            textSize = 16f
            setPadding(0, spacing, 0, 0)
        })
    }

    override fun onResume() {
        super.onResume()
        if (DirectoryInitialization.shouldStart(this)) DirectoryInitialization.start(this)
        AfterDirectoryInitializationRunner().runWithLifecycle(this) {
            chooseButton.isEnabled = true
            gamesDirectory.mkdirs()
            findRom()
        }
    }

    private fun findRom(ignorePrevious: Boolean = false) {
        if (!DirectoryInitialization.areDolphinDirectoriesReady()) return
        val previous = selectedPath ?: preferences.getString("rom", null)
        if (!ignorePrevious && previous != null) {
            selectRom(previous)
            return
        }
        val generation = ++selectionGeneration
        launchButton.isEnabled = false
        lifecycleScope.launch {
            val path = withContext(Dispatchers.IO) {
                gamesDirectory.listFiles()?.sortedBy { it.name }?.firstOrNull {
                    it.isFile && it.extension.lowercase() in ROM_EXTENSIONS && isMetroidPrime(it.path)
                }?.path
            }
            if (generation != selectionGeneration) return@launch
            if (path != null) selectRom(path)
            else status.text = getString(R.string.quest_no_rom, gamesDirectory.absolutePath)
        }
    }

    private fun selectRom(path: String, launch: Boolean = false) {
        val generation = ++selectionGeneration
        selectedPath = path
        launchButton.isEnabled = false
        lifecycleScope.launch {
            val game = withContext(Dispatchers.IO) {
                try {
                    if (path.startsWith("content://")) {
                        contentResolver.openFileDescriptor(Uri.parse(path), "r")?.use { }
                            ?: return@withContext null
                    } else if (!File(path).canRead()) {
                        return@withContext null
                    }
                    GameFile.parse(path)?.takeIf { isSupportedRevision(it) }
                } catch (_: Exception) {
                    null
                }
            }
            if (generation != selectionGeneration) return@launch
            if (game == null) {
                selectedPath = null
                preferences.edit().remove("rom").apply()
                status.text = getString(R.string.quest_invalid_rom)
            } else {
                selectedPath = path
                preferences.edit().putString("rom", path).apply()
                status.text = getString(R.string.quest_ready, game.getTitle(), game.getGameId())
                launchButton.isEnabled = true
                if (launch) {
                    EmulationActivity.launchQuest(this@QuestLauncherActivity, arrayOf(path), false) { message, canRetry ->
                        status.setText(message)
                        launchButton.isEnabled = canRetry
                    }
                }
            }
        }
    }

    private fun isMetroidPrime(path: String): Boolean = try {
        GameFile.parse(path)?.let { isSupportedRevision(it) } == true
    } catch (_: Exception) {
        false
    }

    private fun showGraphicsPresets() {
        val options = resources.getStringArray(R.array.quest_graphics_preset_options)
        val defaultPreset = QuestVrSettings.DEFAULT_GRAPHICS_PRESET
        val defaultOption = options[defaultPreset]
        options[defaultPreset] = getString(
            R.string.quest_graphics_preset_default, defaultOption.substringBefore('\n')
        ) + "\n" + defaultOption.substringAfter('\n')
        AlertDialog.Builder(this)
            .setTitle(R.string.quest_graphics_presets)
            .setItems(options) { _, preset ->
                val applied = if (NativeLibrary.IsUninitialized()) {
                    // Apply first-install defaults before the explicit choice, so
                    // first launch cannot silently overwrite its resolution.
                    Settings().use { settings ->
                        settings.loadSettings(isWii = false)
                        QuestVrSettings.ensureRecommendedDefaults(settings)
                        settings.saveSettings()
                    }
                    NativeLibrary.ApplyQuestGraphicsPreset(preset)
                } else false
                val message = if (applied)
                    R.string.quest_graphics_preset_applied
                else
                    R.string.quest_graphics_preset_failed
                Toast.makeText(this, message, Toast.LENGTH_LONG).show()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun isSupportedRevision(game: GameFile): Boolean =
        game.getGameId() == "GM8E01" && game.getRevision() == 0

    private fun LinearLayout.addButton(label: Int, action: () -> Unit): Button =
        Button(this@QuestLauncherActivity).apply {
            setText(label)
            minHeight = (56 * resources.displayMetrics.density).toInt()
            setOnClickListener { action() }
            addView(this, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        }

    companion object {
        private val ROM_EXTENSIONS = setOf("iso", "ciso", "gcm", "rvz", "gcz", "wbfs")
    }
}
