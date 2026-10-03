// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.fragments

import android.content.Context
import android.graphics.Rect
import android.os.Bundle
import android.view.LayoutInflater
import android.view.SurfaceHolder
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.dolphinemu.dolphinemu.BuildConfig
import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.activities.EmulationActivity
import org.dolphinemu.dolphinemu.databinding.FragmentEmulationBinding
import org.dolphinemu.dolphinemu.features.settings.model.BooleanSetting
import org.dolphinemu.dolphinemu.features.settings.model.Settings
import org.dolphinemu.dolphinemu.features.settings.model.QuestVrSettings
import org.dolphinemu.dolphinemu.overlay.InputOverlay
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.Log
import java.io.File

class EmulationFragment : Fragment(), SurfaceHolder.Callback {
    private var inputOverlay: InputOverlay? = null

    private var gamePaths: Array<String>? = null
    private var riivolution = false
    private var runWhenSurfaceIsValid = false
    private var surfaceForwarded = false
    private var loadPreviousTemporaryState = false
    private var launchSystemMenu = false

    private var emulationActivity: EmulationActivity? = null
    private var resumeJob: Job? = null

    private var _binding: FragmentEmulationBinding? = null
    private val binding get() = _binding!!

    override fun onAttach(context: Context) {
        super.onAttach(context)
        if (context is EmulationActivity) {
            emulationActivity = context
            if (!BuildConfig.IS_QUEST || EmulationActivity.questSession.canResume(context) ||
                EmulationActivity.questSession.isReadyForLaunch()) {
                NativeLibrary.setEmulationActivity(context)
            }
        } else {
            throw IllegalStateException("EmulationFragment must have EmulationActivity parent")
        }
    }

    /**
     * Initialize anything that doesn't depend on the layout / views in here.
     */
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requireArguments().apply {
            gamePaths = getStringArray(KEY_GAMEPATHS)
            riivolution = getBoolean(KEY_RIIVOLUTION)
            launchSystemMenu = getBoolean(KEY_SYSTEM_MENU)
        }
    }

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
    ): View {
        _binding = FragmentEmulationBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        // The new Surface created here will get passed to the native code via onSurfaceChanged.
        val surfaceView = binding.surfaceEmulation
        surfaceView.holder.addCallback(this)

        inputOverlay = binding.surfaceInputOverlay

        val doneButton = binding.doneControlConfig
        doneButton?.setOnClickListener { stopConfiguringControls() }

        if (inputOverlay != null) {
            view.post {
                val overlayX = inputOverlay!!.left
                val overlayY = inputOverlay!!.top
                inputOverlay?.setSurfacePosition(
                    Rect(
                        surfaceView.left - overlayX,
                        surfaceView.top - overlayY,
                        surfaceView.right - overlayX,
                        surfaceView.bottom - overlayY
                    )
                )
            }
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    override fun onResume() {
        super.onResume()
        if (NativeLibrary.IsGameMetadataValid()) {
            inputOverlay?.refreshControls()
        }

        AfterDirectoryInitializationRunner().runWithLifecycle(this) {
            val activity = emulationActivity ?: return@runWithLifecycle
            if (BuildConfig.IS_QUEST) {
                resumeJob?.cancel()
                resumeJob = lifecycleScope.launch {
                    val session = EmulationActivity.questSession
                    if (!session.canResume(activity) && !session.isReadyForLaunch()) {
                        Toast.makeText(activity, R.string.quest_recovering_vr, Toast.LENGTH_LONG).show()
                        session.requestStop()
                        if (!EmulationActivity.awaitQuestSessionStopped()) {
                            Toast.makeText(activity, R.string.quest_recovery_failed, Toast.LENGTH_LONG).show()
                            activity.finish()
                            return@launch
                        }
                    }
                    if (isResumed && !activity.isFinishing && !activity.isDestroyed) {
                        // A recreated Activity cannot inherit the old Activity's OpenXR instance.
                        // Register it only after the previous JNI Run/finish callback has returned.
                        NativeLibrary.setEmulationActivity(activity)
                        val surface = binding.surfaceEmulation.holder.surface
                        if (surface.isValid && !surfaceForwarded) {
                            NativeLibrary.SurfaceChanged(surface)
                            surfaceForwarded = true
                        }
                        run(activity.isActivityRecreated)
                    }
                }
            } else {
                run(activity.isActivityRecreated)
            }
        }
    }

    override fun onPause() {
        resumeJob?.cancel()
        if ((!BuildConfig.IS_QUEST || emulationActivity?.let {
                EmulationActivity.questSession.ownsRun(it)
            } == true) && NativeLibrary.IsRunningAndUnpaused() && !NativeLibrary.IsShowingAlertMessage()) {
            Log.debug("[EmulationFragment] Pausing emulation.")
            NativeLibrary.PauseEmulation(true)
        }
        super.onPause()
    }

    override fun onDestroy() {
        inputOverlay?.onDestroy()
        super.onDestroy()
    }

    override fun onDetach() {
        emulationActivity?.let { NativeLibrary.clearEmulationActivity(it) }
        emulationActivity = null
        super.onDetach()
    }

    fun toggleInputOverlayVisibility(settings: Settings?) {
        BooleanSetting.MAIN_SHOW_INPUT_OVERLAY.setBoolean(
            settings!!, !BooleanSetting.MAIN_SHOW_INPUT_OVERLAY.boolean
        )

        inputOverlay?.refreshControls()
    }

    fun initInputPointer() = inputOverlay?.initTouchPointer()

    fun refreshInputOverlay() = inputOverlay?.refreshControls()

    fun refreshOverlayPointer() = inputOverlay?.refreshOverlayPointer()

    fun resetInputOverlay() = inputOverlay?.resetButtonPlacement()

    override fun surfaceCreated(holder: SurfaceHolder) {
        // We purposely don't do anything here.
        // All work is done in surfaceChanged, which we are guaranteed to get even for surface creation.
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.debug("[EmulationFragment] Surface changed. Resolution: $width x $height")
        if (BuildConfig.IS_QUEST && emulationActivity?.let {
                EmulationActivity.questSession.canResume(it)
            } != true && !EmulationActivity.questSession.isReadyForLaunch()) return
        NativeLibrary.SurfaceChanged(holder.surface)
        surfaceForwarded = true
        if (runWhenSurfaceIsValid) {
            runWithValidSurface()
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.debug("[EmulationFragment] Surface destroyed.")
        if (!BuildConfig.IS_QUEST || (surfaceForwarded && emulationActivity != null &&
                NativeLibrary.getEmulationActivity() === emulationActivity))
            NativeLibrary.SurfaceDestroyed()
        surfaceForwarded = false
        runWhenSurfaceIsValid = true
    }

    fun stopEmulation() {
        Log.debug("[EmulationFragment] Stopping emulation.")
        NativeLibrary.StopEmulation()
    }

    fun startConfiguringControls() {
        binding.doneControlConfig?.visibility = View.VISIBLE
        inputOverlay?.editMode = true
    }

    fun stopConfiguringControls() {
        binding.doneControlConfig?.visibility = View.GONE
        inputOverlay?.editMode = false
    }

    val isConfiguringControls: Boolean
        get() = inputOverlay != null && inputOverlay!!.isInEditMode

    private fun run(isActivityRecreated: Boolean) {
        if (isActivityRecreated) {
            if (NativeLibrary.IsUninitialized()) {
                loadPreviousTemporaryState = true
            } else {
                loadPreviousTemporaryState = false
                deleteFile(temporaryStateFilePath)
            }
        } else {
            Log.debug("[EmulationFragment] activity resumed or fresh start")
            loadPreviousTemporaryState = false
            // activity resumed without being killed or this is the first run
            deleteFile(temporaryStateFilePath)
        }

        // If the surface is set, run now. Otherwise, wait for it to get set.
        if (NativeLibrary.HasSurface()) {
            runWithValidSurface()
        } else {
            runWhenSurfaceIsValid = true
        }
    }

    private fun runWithValidSurface() {
        runWhenSurfaceIsValid = false
        val activity = emulationActivity ?: return
        val session = EmulationActivity.questSession
        if (BuildConfig.IS_QUEST && !session.canResume(activity) && !session.isReadyForLaunch())
            return
        if (NativeLibrary.IsUninitialized()) {
            // Initialization must finish before profiles/config can be read. This path also
            // covers Android restoring the activity after killing the process.
            Settings().use { settings ->
                settings.loadSettings()
                QuestVrSettings.prepareLaunchSettings(settings, launchSystemMenu)
                settings.saveSettings()
            }
            // Capture arguments on the UI thread. A detached fragment must not be accessed
            // by the native worker, which can outlive its Activity during shutdown.
            val paths = if (launchSystemMenu) null else requireNotNull(gamePaths).clone()
            val fromTemporaryState = loadPreviousTemporaryState
            val statePath = if (fromTemporaryState) temporaryStateFilePath else null
            val systemMenu = launchSystemMenu
            val useRiivolution = riivolution
            val emulationThread = Thread({
                try {
                    if (BuildConfig.IS_QUEST) NativeLibrary.SetIsBooting()
                    if (fromTemporaryState) {
                        Log.debug("[EmulationFragment] Starting emulation thread from previous state.")
                        NativeLibrary.Run(requireNotNull(paths), useRiivolution, requireNotNull(statePath), true)
                    } else if (systemMenu) {
                        Log.debug("[EmulationFragment] Starting emulation thread for the Wii Menu.")
                        NativeLibrary.RunSystemMenu()
                    } else {
                        Log.debug("[EmulationFragment] Starting emulation thread.")
                        NativeLibrary.Run(requireNotNull(paths), useRiivolution)
                    }
                } finally {
                    // Release the old launch guard before allowing a new Quest Run. Core's
                    // Uninitialized state alone precedes JNI/XR cleanup and is insufficient.
                    EmulationActivity.stopIgnoringLaunchRequests()
                    if (BuildConfig.IS_QUEST) session.finishRun(activity)
                }
            }, "NativeEmulation")
            if (BuildConfig.IS_QUEST && !session.beginRun(activity)) return
            if (!BuildConfig.IS_QUEST) NativeLibrary.SetIsBooting()
            try {
                emulationThread.start()
            } catch (error: Throwable) {
                EmulationActivity.stopIgnoringLaunchRequests()
                if (BuildConfig.IS_QUEST) session.finishRun(activity)
                throw error
            }
        } else {
            if (!EmulationActivity.hasUserPausedEmulation && !NativeLibrary.IsShowingAlertMessage()) {
                Log.debug("[EmulationFragment] Resuming emulation.")
                NativeLibrary.UnPauseEmulation()
            }
        }
    }

    fun saveTemporaryState() = NativeLibrary.SaveStateAs(temporaryStateFilePath)

    private val temporaryStateFilePath: String
        get() = "${requireContext().filesDir}${File.separator}temp.sav"

    companion object {
        private const val KEY_GAMEPATHS = "gamepaths"
        private const val KEY_RIIVOLUTION = "riivolution"
        private const val KEY_SYSTEM_MENU = "systemMenu"

        fun newInstance(
            gamePaths: Array<String>?, riivolution: Boolean, systemMenu: Boolean
        ): EmulationFragment {
            val args = Bundle()
            args.apply {
                putStringArray(KEY_GAMEPATHS, gamePaths)
                putBoolean(KEY_RIIVOLUTION, riivolution)
                putBoolean(KEY_SYSTEM_MENU, systemMenu)
            }
            val fragment = EmulationFragment()
            fragment.arguments = args
            return fragment
        }

        private fun deleteFile(path: String) {
            try {
                val file = File(path)
                if (!file.delete()) {
                    Log.error("[EmulationFragment] Failed to delete ${file.absolutePath}")
                }
            } catch (ignored: Exception) {
            }
        }
    }
}
