package com.kepyur.app

import android.os.SystemClock
import android.view.Surface
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.abs

/**
 * The native side, faked: records every call in order, answers [open] / [usb3Switch] / [superspeed] as the test
 * scripts, and keeps a small model of the radio from which [status] builds the same JSON keys as
 * native/radio/radio.cpp (Radio::status_json).
 *
 * Mimics the real radio where the app depends on it: [watch] and [scan] take effect at once (the JNI calls set the
 * mode under the lock before returning) and only while open; [open] resets the bank counter and the dead flag and
 * leaves the mode idle; [close] marks it closed and idle; a scan ends (when the test says [finishScan]) on the
 * preferred channel if it was found, else on the strongest found, else on the preferred one, else idle.
 *
 * Called from the main thread (status, settings, tuning) and from UsbLink's worker (usb3Switch, open, close): every
 * method is synchronized, except that a held [open] waits outside the lock.
 */
class FakeEngine : Engine {
    data class Call(val name: String, val args: List<Any?>) {
        override fun toString() = "$name(${args.joinToString()})"
    }

    data class Pilot(val mhz: Double, val name: String, val snr: Double, val std: String = "NTSC")

    private val log = ArrayList<Call>()

    // ------------------------------------------------------------------------------------- scripting
    /** What successive [open] calls return ("" = success); once used up, [openResult]. */
    val openResults = ArrayDeque<String>()
    @Volatile var openResult = ""
    /** What successive [usb3Switch] calls return; once used up, [switchResult]. */
    val switchResults = ArrayDeque<String>()
    @Volatile var switchResult = "not_needed"
    @Volatile var superspeed = true
    /** Raw strings [status] returns first, one per call (a malformed status). */
    val rawStatus = ArrayDeque<String>()

    /** While set, [open] waits for [release] before doing anything. */
    @Volatile private var gate: CountDownLatch? = null
    @Volatile var insideOpen = 0
        private set

    fun hold() { gate = CountDownLatch(1) }
    fun release() { gate?.countDown(); gate = null }

    // ------------------------------------------------------------------------------------- the radio's model
    var isOpen = false
    var mode = "idle"
    var watchHz = 0.0
    var reqHz = 0.0
    var banks = 0L
    /** The bank counter stops (a stream that has silently stalled). */
    var freezeBanks = false
    var dead = false
    /** The radio stopped because the USB data path wedged ("wedged" in the status, with "dead"). Cleared by [open]. */
    var wedged = false
    /** null: "streaming" while open, else "closed". */
    var rxState: String? = null
    var rxError = ""
    var locked = false
    var videoStd = "NTSC"
    var dropped = 0L
    /** Blocks lost on the USB link itself ("stream_dropped" in the status). */
    var streamDropped = 0L
    var dspLoad = 0.3
    val pilots = ArrayList<Pilot>()
    /**
     * When each pilot (by frequency) was last seen, on the fake clock: as the real radio, which keeps every pilot it has
     * ever seen and reports "age_s", the whole seconds since it last saw each. A pilot added to [pilots] by hand has
     * age 0 (always fresh). [agePilots] makes them all older at once.
     */
    val seenAt = HashMap<Double, Long>()

    @Synchronized
    fun agePilots(seconds: Long) {
        val now = SystemClock.elapsedRealtime()
        for (p in pilots) seenAt[p.mhz] = (seenAt[p.mhz] ?: now) - seconds * 1000
    }
    /** The stream test's result object ("test" in the status), null until a test has run. */
    var testResult: JSONObject? = null
    private var resumeHz = 0.0
    var statusCalls = 0
        private set

    // ------------------------------------------------------------------------------------- the log
    private fun rec(name: String, vararg args: Any?) = synchronized(this) { log += Call(name, args.toList()) }

    val calls: List<Call> get() = synchronized(this) { log.toList() }
    fun calls(name: String) = calls.filter { it.name == name }
    fun count(name: String) = calls(name).size
    fun last(name: String) = calls(name).lastOrNull()
    /** The calls that matter for the radio's state, without the status polls and log lines. */
    fun radioCalls(): List<Call> = calls.filter { it.name !in setOf("status", "log") }
    fun clearCalls() = synchronized(this) { log.clear() }

    /** The scan in progress ends: the radio watches what the real one would choose, or goes idle. */
    @Synchronized
    fun finishScan(found: List<Pilot>) {
        check(mode == "scan") { "finishScan: the fake is not scanning (mode $mode)" }
        for (f in found) {
            pilots.removeAll { abs(it.mhz - f.mhz) < 4 }
            pilots += f
            seenAt[f.mhz] = SystemClock.elapsedRealtime()
        }
        pilots.sortByDescending { it.snr }
        var next = 0.0
        if (resumeHz > 0) found.firstOrNull { abs(it.mhz * 1e6 - resumeHz) < 4e6 }?.let { next = it.mhz * 1e6 }
        if (next == 0.0) found.maxByOrNull { it.snr }?.let { next = it.mhz * 1e6 }
        if (next == 0.0 && resumeHz > 0) next = resumeHz
        if (next > 0) {
            watchHz = next
            mode = "watch"
        } else {
            mode = "idle"
        }
    }

    /** The stream test in progress has run its time: the radio goes idle and says so in "test", as the real one does. */
    @Synchronized
    fun finishStreamTest(rateMsps: Double) {
        check(mode == "test") { "finishStreamTest: the fake is not testing (mode $mode)" }
        testResult = JSONObject().put("rate_msps", rateMsps).put("seconds", 60.0).put("banks", 1000).put("expected", 1000)
            .put("seq_gaps", 0).put("stream_runs", 1).put("restarts", 0).put("queue_dropped", 0).put("done", true)
        mode = "idle"
    }

    // ------------------------------------------------------------------------------------- Engine
    override fun init(logPath: String) = rec("init", logPath)

    @Synchronized override fun usb3Switch(fd: Int): String {
        rec("usb3Switch", fd)
        return switchResults.removeFirstOrNull() ?: switchResult
    }

    override fun open(fd: Int): String {
        synchronized(this) { insideOpen++; rec("open", fd) }
        try {
            gate?.let { check(it.await(10, TimeUnit.SECONDS)) { "FakeEngine.open was held and never released" } }
        } finally {
            synchronized(this) { insideOpen-- }
        }
        synchronized(this) {
            val err = openResults.removeFirstOrNull() ?: openResult
            if (err.isEmpty()) {
                isOpen = true
                mode = "idle"
                banks = 0
                dead = false
                wedged = false
                rxState = null
            }
            return err
        }
    }

    @Synchronized override fun close() {
        rec("close")
        isOpen = false
        mode = "idle"
    }

    @Synchronized override fun superspeed(): Boolean { rec("superspeed"); return superspeed }

    @Synchronized override fun scan(preferHz: Double) {
        rec("scan", preferHz)
        if (!isOpen) return
        resumeHz = if (preferHz > 0) preferHz else if (mode == "watch") watchHz else 0.0
        mode = "scan"
    }

    @Synchronized override fun watch(hz: Double, fresh: Boolean) {
        rec("watch", hz, fresh)
        if (!isOpen) return
        watchHz = hz
        mode = "watch"
    }

    @Synchronized override fun idle() {
        rec("idle")
        if (isOpen) mode = "idle"
    }

    @Synchronized override fun streamTest(rateSel: Int, seconds: Double) {
        rec("streamTest", rateSel, seconds)
        if (isOpen) mode = "test"
    }

    @Synchronized override fun setWatchRate(rateSel: Int) = rec("setWatchRate", rateSel)
    @Synchronized override fun setGain(db: Double) = rec("setGain", db)
    @Synchronized override fun setComb(on: Boolean) = rec("setComb", on)
    @Synchronized override fun setDiversity(on: Boolean) = rec("setDiversity", on)

    private var offset = false
    @Synchronized override fun setOffsetTuning(on: Boolean): Boolean {
        rec("setOffsetTuning", on)
        val changed = offset != on
        offset = on
        return changed
    }

    @Synchronized override fun setPicture(lumaComb: Boolean, adaptiveComb: Boolean, smoothLines: Boolean, repair: Boolean) =
        rec("setPicture", lumaComb, adaptiveComb, smoothLines, repair)

    @Synchronized override fun setView(aspect: Float, fit: Int, scale: Float, stereo: Boolean, eyeShift: Float) =
        rec("setView", aspect, fit, scale, stereo, eyeShift)

    @Synchronized override fun setBackdrop(on: Boolean, now: Boolean) = rec("setBackdrop", on, now)
    @Synchronized override fun setCardLabel(label: String) = rec("setCardLabel", label)
    @Synchronized override fun setPreview(left: Int, top: Int, right: Int, bottom: Int, radius: Float) =
        rec("setPreview", left, top, right, bottom, radius)
    @Synchronized override fun record(seconds: Double, prefix: String) = rec("record", seconds, prefix)
    @Synchronized override fun setSurface(surface: Surface?) = rec("setSurface", surface != null)
    @Synchronized override fun redraw() = rec("redraw")
    @Synchronized override fun testCard(on: Boolean) = rec("testCard", on)
    @Synchronized override fun log(msg: String) = rec("log", msg)
    @Synchronized override fun holdPicture(ms: Int) = rec("holdPicture", ms)

    @Synchronized override fun status(): String {
        rec("status")
        statusCalls++
        rawStatus.removeFirstOrNull()?.let { return it }
        if (isOpen && !freezeBanks) banks += 10
        val o = JSONObject()
        o.put("open", isOpen)
        o.put("usb3", superspeed)
        o.put("chip", "RTL8812BU")
        o.put("rx_state", rxState ?: if (isOpen) "streaming" else "closed")
        o.put("rx_error", rxError)
        o.put("last_fault", "")
        o.put("mode", mode)
        o.put("freq_hz", reqHz)
        o.put("watch_hz", watchHz)
        o.put("fs", 40e6)
        o.put("banks", banks)
        o.put("dropped", dropped)
        o.put("stream_dropped", streamDropped)
        o.put("decode_dropped", 0)
        o.put("restarts", 0)
        o.put("dsp_load", dspLoad)
        o.put("decode_load", dspLoad)
        o.put("watch_msps", if (mode == "watch") 40 else 0)
        o.put("fallback", false)
        o.put("dead", dead || wedged)
        o.put("wedged", wedged)
        o.put("offset_tuning", offset)
        o.put("centre_hz", 0)
        if (mode == "watch" && locked) o.put("snr_db", 27.0)
        if (mode == "scan") { o.put("scan_step", 3); o.put("scan_steps", 29) }
        o.put("video", JSONObject().apply {
            put("locked", locked); put("lock", if (locked) 0.95 else 0.0); put("std", if (locked) videoStd else "")
            put("fps", if (locked) 59.94 else 0.0); put("acquisitions", 0); put("losses", 0); put("rephases", 0)
            put("clicks_repaired", 0); put("lines_mended", 0); put("colour", locked)
        })
        o.put("antennas", JSONObject().apply {
            put("combine", false); put("snr_a", 0); put("snr_b", 0); put("snr_c", 0); put("b_share", 0)
            put("a_fade", 0); put("rescued", 0); put("coherence", 0)
        })
        o.put("test", testResult ?: JSONObject.NULL)
        o.put("pilots", JSONArray().apply {
            for (p in pilots) put(JSONObject().apply {
                put("mhz", p.mhz); put("name", p.name); put("std", p.std); put("snr", p.snr); put("age_s", seenAt[p.mhz]?.let { (SystemClock.elapsedRealtime() - it) / 1000 } ?: 0L)
            })
        })
        return o.toString()
    }
}
