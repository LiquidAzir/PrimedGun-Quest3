// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

/** Tracks the entire JNI Run call, including teardown after the core becomes uninitialized. */
class QuestSessionLifecycle(
    private val isCoreUninitialized: () -> Boolean,
    private val isCoreRunning: () -> Boolean,
    private val stopCore: () -> Unit,
    private val onStopFailure: (Exception) -> Unit = {}
) {
    private var runOwner: Any? = null
    private var launchPending = false
    private var stopPending = false

    @Synchronized
    fun beginLaunch(): Boolean {
        if (launchPending) return false
        launchPending = true
        return true
    }

    @Synchronized
    fun finishLaunch() {
        launchPending = false
    }

    @Synchronized
    fun beginRun(owner: Any): Boolean {
        if (!isReadyForLaunch()) return false
        runOwner = owner
        return true
    }

    @Synchronized
    fun finishRun(owner: Any) {
        if (runOwner === owner) runOwner = null
    }

    @Synchronized
    fun ownsRun(owner: Any): Boolean = runOwner === owner

    @Synchronized
    fun canResume(owner: Any): Boolean =
        runOwner === owner && !stopPending && !isCoreUninitialized()

    @Synchronized
    fun isReadyForLaunch(): Boolean =
        runOwner == null && !stopPending && isCoreUninitialized()

    /** Never block the UI on Core::Stop's host lock or allow a second stop worker. */
    @Synchronized
    fun requestStop() {
        if (stopPending || (runOwner == null && isCoreUninitialized())) return
        stopPending = true
        try {
            Thread({
                try {
                    // SetIsBooting/BootCore may not have run yet. Stopping an uninitialized
                    // core then would do nothing and leave the forthcoming session orphaned.
                    while (hasRun() && !isCoreRunning()) Thread.sleep(25)
                    if (!isCoreUninitialized()) stopCore()
                } catch (error: Exception) {
                    onStopFailure(error)
                } finally {
                    synchronized(this) { stopPending = false }
                }
            }, "QuestSessionStop").start()
        } catch (error: Throwable) {
            stopPending = false
            throw error
        }
    }

    @Synchronized
    private fun hasRun(): Boolean = runOwner != null
}
