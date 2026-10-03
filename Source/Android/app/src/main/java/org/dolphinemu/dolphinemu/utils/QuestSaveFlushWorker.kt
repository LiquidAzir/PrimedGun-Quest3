// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

/**
 * Serializes process-owned save flushes without blocking Android lifecycle callbacks.
 * A request made during a flush always schedules another pass; bursts are coalesced.
 */
class QuestSaveFlushWorker(
    private val flush: () -> Unit,
    private val onFailure: (Exception) -> Unit
) {
    private val lock = Any()
    private var pending = false
    private var running = false

    fun requestFlush() {
        synchronized(lock) {
            pending = true
            if (running) return
            running = true
        }

        try {
            Thread(::drainRequests, "QuestSaveFlush").apply { isDaemon = true }.start()
        } catch (error: Exception) {
            // Keep the request pending so a later lifecycle event can retry it.
            synchronized(lock) { running = false }
            reportFailure(error)
        }
    }

    private fun drainRequests() {
        while (true) {
            synchronized(lock) {
                if (!pending) {
                    running = false
                    return
                }
                pending = false
            }
            try {
                // The JNI function retains its host lock and normal save synchronization.
                // Never hold our queue lock while the native flush is waiting.
                flush()
            } catch (error: Exception) {
                reportFailure(error)
            }
        }
    }

    private fun reportFailure(error: Exception) {
        try {
            onFailure(error)
        } catch (_: Exception) {
            // An error reporter must not strand subsequent save-flush requests.
        }
    }
}
