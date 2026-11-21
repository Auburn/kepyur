package com.kepyur.app

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.view.animation.DecelerateInterpolator
import android.widget.FrameLayout
import org.json.JSONObject
import java.io.File
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * kepyur: one screen. The picture fills it; the menu ([MenuView]) lies over the picture, blurred behind it,
 * and is open when the app starts. With the menu closed there is nothing on screen but the picture: a tap
 * brings up a MENU button for three seconds.
 *
 * USB stream test (for the first hardware check):  adb shell am start -n com.kepyur.app/.MainActivity --es mode usbtest
 */
/**
 * Where the log and the recordings go: the app's folder on shared storage (Android/data/com.kepyur.app/files,
 * readable over USB without root), or its private folder when shared storage is not there. Android answers null
 * then, which happens: for the first seconds after a boot, and whenever the volume is not mounted.
 */
internal fun Context.dataDir(): File = getExternalFilesDir(null) ?: filesDir

class MainActivity : Activity(), MenuView.Listener, UsbLink.Events {
    private val ui = Handler(Looper.getMainLooper())
    private val engine = Engine.get()
    private lateinit var settings: Settings
    private lateinit var link: UsbLink

    private lateinit var root: FrameLayout
    private lateinit var video: SurfaceView
    private lateinit var menu: MenuView
    private lateinit var hint: MenuHint
    private var menuOpen = true
    /**
     * The menu opened itself, because there was no picture to show and a reason to give (the adapter gone, a
     * radio being started again): it closes itself when the picture is back, unless it has been touched since.
     */
    private var menuAuto = false
    private var foreground = false
    private var wasRunning = false
    private val surfaceToken = Any()

    // what the radio last said
    private var mode = ""
    private var found: List<Found> = emptyList()
    private var noVideoFound = false       // the last scan came back empty
    private var lastDropped = 0L
    private var overloadedUntil = 0L
    // blocks the adapter's stream gave up on (collected too late): the count at the last refresh, when that was
    // and whether the radio was watching then, and for each rise seen while watching in the last LATE_WINDOW_MS,
    // when and by how many (see refresh())
    private var lastLate = 0L
    private var lastLateAt = 0L
    private var lastLateWatching = false
    private val lateMarks = ArrayDeque<Pair<Long, Long>>()
    private var frameRateFor = ""
    private var appliedRate = 0
    private var scanOnReturn = false       // a scan is owed: the first one, or one the background cut short
    private var scanStartedAt = 0L         // when the last scan began (elapsedRealtime); 0: none this session
    private var statusBroken = false

    // USB stream test
    private val testPlan = ArrayDeque<Pair<Int, Double>>()
    private var testRunning = false
    private val testResults = mutableListOf<String>()
    private var testRequested = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        settings = Settings(this)
        engine.init(dataDir().absolutePath + "/kepyur.log")
        engine.log("start, ${Build.MANUFACTURER} ${Build.MODEL} Android ${Build.VERSION.RELEASE}")
        applyPicture()
        applySignal()
        applyView()
        engine.setBackdrop(true, true)
        buildUi()
        link = UsbLink(this, engine, this)
        handleIntent(intent)
        link.start()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
        if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) link.onAttachedIntent()
    }

    /**
     * adb shell am start -n com.kepyur.app/.MainActivity [--es mode usbtest] [--ef gain 30] [--ef record 3] [--ez testcard true]
     * USB debugging: [--es env WSDR_URB_DEPTH=64,OTHER=1] (wifi-sdr / devourer switches, read at the next open or stream
     * start; KEY= removes one), [--ez soak true] (a 40 MS/s stream test after every open instead of watching: how long
     * the link holds up, open after open).
     */
    private fun handleIntent(i: Intent?) {
        i ?: return
        if (i.hasExtra("testcard")) engine.testCard(i.getBooleanExtra("testcard", false))
        i.getStringExtra("env")?.split(',')?.forEach { kv ->
            val k = kv.substringBefore('=').trim()
            if (k.isNotEmpty()) engine.setDebugEnv(k, kv.substringAfter('=', "").trim())
        }
        if (i.hasExtra("soak")) {
            soak = i.getBooleanExtra("soak", false)
            engine.log("debug: soak $soak")
        }
        if (i.getStringExtra("mode") == "usbtest") {
            testRequested = true
            if (::link.isInitialized && link.state == UsbLink.State.RUNNING) startTest()
        }
        if (i.hasExtra("gain")) {   // the same as the menu's Gain: below 0 automatic, else that many dB
            val g = i.getFloatExtra("gain", -1f)
            settings.gainAuto = g < 0
            if (g >= 0) settings.gainDb = g.coerceAtMost(Settings.GAIN_MAX)
            settings.save()
            engine.log("debug: gain ${if (g < 0) "automatic" else "$g dB"}")
            if (::menu.isInitialized) menu.syncGain()
            applySignal()
            retune()
        }
        if (i.hasExtra("record")) {
            val secs = i.getFloatExtra("record", 3f).toDouble()
            val name = "rec-" + java.text.SimpleDateFormat("HHmmss", java.util.Locale.US).format(java.util.Date())
            engine.record(secs, dataDir().absolutePath + "/" + name)
        }
    }

    override fun onStart() {
        super.onStart()
        foreground = true
        ui.removeCallbacks(closeInBackground)
        ui.removeCallbacks(poll)
        ui.post(poll)
        if (!link.started) {
            // the adapter was let go in the background: it takes a few seconds to come back (onRadioUp then
            // tunes), and until then the menu says so instead of a black screen saying nothing
            link.start()
            showMenuForTrouble()
        }
        // back from the background, where decoding was stopped
        if (link.state == UsbLink.State.RUNNING) {
            if (!testRunning) {
                if (scanOnReturn) onScan() else if (mode == "idle") tune(fresh = true)
            }
        } else {
            showMenuForTrouble()   // the adapter went, or the radio died, while nobody was looking
        }
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    override fun onStop() {
        foreground = false
        ui.removeCallbacks(poll)
        // nobody is watching: no reason to keep two cores decoding
        if (link.state == UsbLink.State.RUNNING && !testRunning) {
            if (mode == "scan") scanOnReturn = true   // cut short: start it over when the app is back
            engine.idle()
            mode = "idle"
        }
        // ... nor, after a while, to keep the adapter streaming and warm: it is opened again on return
        if (!testRunning) ui.postDelayed(closeInBackground, BACKGROUND_CLOSE_MS)
        super.onStop()
    }

    private val closeInBackground = Runnable {
        engine.log("in the background for ${BACKGROUND_CLOSE_MS / 1000} s: closing the adapter")
        link.stop()
        mode = ""
        wasRunning = false
    }

    override fun onDestroy() {
        ui.removeCallbacksAndMessages(null)
        link.stop()
        super.onDestroy()
    }

    /** Back goes up: from the picture to the menu, from the menu out of the app. */
    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (menuOpen) {
            @Suppress("DEPRECATION") super.onBackPressed()
        } else openMenu()
    }

    // ---------------------------------------------------------------------------------------- settings -> native
    private fun applyPicture() {
        engine.setPicture(settings.lumaComb, settings.lumaComb && settings.cleanOsd, settings.smoothLines, settings.sparkleRepair)
        engine.setComb(settings.colourComb)
    }

    /** True when something changed that only a new tune applies. */
    private fun applySignal(): Boolean {
        // only when it changed: setting the rate also forgets that 40 had to fall back to 20
        val rate = if (settings.rate40) 1 else 2
        if (rate != appliedRate) engine.setWatchRate(rate)
        appliedRate = rate
        engine.setGain(if (settings.gainAuto) -1.0 else settings.gainDb.toDouble())
        engine.setDiversity(settings.diversity)
        return engine.setOffsetTuning(settings.offsetTune)
    }

    private fun applyView() =
        engine.setView(settings.aspect, settings.fit, settings.scale, settings.goggles, settings.eyeShift)

    /** The frequency for the chosen channel: where a scan measured its transmitter, if it found one, else the nominal one. */
    private fun channelHz(): Double {
        val mhz = settings.channelMhz
        if (mhz <= 0) return 0.0
        val seen = found.filter { abs(it.mhz - mhz) < Channels.SAME_CHANNEL_MHZ }.maxByOrNull { it.snrDb }
        return (seen?.mhz ?: mhz.toDouble()) * 1e6
    }

    private fun tune(fresh: Boolean) {
        if (link.state != UsbLink.State.RUNNING || testRunning) return
        val hz = channelHz()
        if (hz <= 0) return
        noVideoFound = false
        scanOnReturn = false   // a channel has been chosen since: no scan is owed any more
        engine.watch(hz, fresh)
        mode = "watch"
    }

    /** The same channel again, for a setting that only takes effect on a tune. */
    private fun retune() {
        if (mode == "watch") tune(fresh = false)
    }

    // ---------------------------------------------------------------------------------------- MenuView.Listener
    override fun onTune() = tune(fresh = true)

    override fun onScan() {
        if (link.state != UsbLink.State.RUNNING || testRunning) return
        scanOnReturn = false
        noVideoFound = false
        scanStartedAt = android.os.SystemClock.elapsedRealtime()
        found = emptyList()   // what earlier scans saw is history: this one starts from nothing
        engine.scan(channelHz())
        mode = "scan"
        refresh()
    }

    override fun onView() = applyView()

    override fun onPicture(rebuild: Boolean) {
        applyPicture()
        if (rebuild) retune()
    }

    override fun onSignal(retune: Boolean) {
        applySignal()
        if (retune) retune()
    }

    /** A view slider is being dragged: the picture sharp under it, so the change can be judged. */
    override fun onPeek(on: Boolean) = engine.setBackdrop(menuOpen && !on, false)

    /** A finger on the menu: whoever opened it, it now stays until it is closed. */
    override fun onTouched() {
        menuAuto = false
    }

    override fun onStatusAction() = link.retryNow()

    override fun onClose() = closeMenu()

    // ---------------------------------------------------------------------------------------- UsbLink.Events
    override fun onLinkChanged() {
        val running = link.state == UsbLink.State.RUNNING
        if (!running) {
            mode = ""
            testRunning = false
            // the picture has just gone, with nothing on screen to say why: the menu does (and steps aside
            // again by itself once the radio is back, see onRadioUp). Not for the few seconds of a reopen after the
            // USB data path wedged: on some phones that comes every minute or so, and fixes itself.
            if (wasRunning && foreground && !link.reopeningAfterWedge) showMenuForTrouble()
        }
        wasRunning = running
        refresh()
    }

    private fun showMenuForTrouble() {
        if (menuOpen || !::menu.isInitialized) return
        openMenu()
        menuAuto = true
    }

    override fun onRadioUp() {
        applyPicture()
        applySignal()
        lastDropped = 0
        lastLate = 0
        lastLateWatching = false
        lateMarks.clear()
        if (menuAuto) closeMenu()   // it only came up to say the picture was away
        when {
            testRequested || soak -> startTest()
            !foreground -> {   // it came up with nobody watching: wait, and on return tune, or scan if this is the first run
                engine.idle()
                mode = "idle"
                scanOnReturn = settings.channelMhz <= 0
            }
            scanOnReturn -> onScan()   // one was cut short before the adapter was let go
            settings.channelMhz > 0 -> tune(fresh = true)
            else -> onScan()   // first run: find something to show
        }
        refresh()
    }

    // ---------------------------------------------------------------------------------------- USB stream test
    private fun startTest() {
        testRequested = false
        testPlan.clear(); testResults.clear()
        if (soak) {
            testPlan.add(1 to SOAK_S)   // 40 MS/s until it fails (the next open starts another)
        } else {
            testPlan.add(1 to 60.0)   // 40 MS/s for 60 s
            testPlan.add(2 to 60.0)   // 20 MS/s for 60 s
        }
        nextTest()
    }

    private fun nextTest() {
        val t = testPlan.removeFirstOrNull()
        if (t == null) {
            testRunning = false
            engine.log("usbtest: all done: ${testResults.joinToString(" | ")}")
            engine.log("usbtest: now decoding for the rest of the session")
            applySignal()
            if (settings.channelMhz > 0) tune(fresh = true) else onScan()
            return
        }
        testRunning = true
        engine.log("usbtest: rate_sel ${t.first} for ${t.second} s")
        engine.streamTest(t.first, t.second)
        mode = "test"
    }

    // ---------------------------------------------------------------------------------------- status
    private val poll = object : Runnable {
        override fun run() {
            refresh()
            ui.postDelayed(this, 250)
        }
    }

    private fun refresh() {
        if (!::menu.isInitialized) return
        val st = if (link.state == UsbLink.State.RUNNING) radioState() else linkState()
        if (menuOpen) menu.render(st)
    }

    /**
     * A number from the status, or [or] when it is missing or not a finite number. (The native side prints
     * doubles as they come, and Android's JSON reader takes a bare nan or inf for a string without complaint.)
     */
    private fun JSONObject.num(key: String, or: Double = 0.0): Double = optDouble(key, or).let { if (it.isFinite()) it else or }

    private fun linkState(): MenuState = linkHeadline().let { st ->
        // the rail may have to cut a long message short on a small screen: the Signal tab shows all of it
        if (st.tone == Tone.WARN || st.tone == Tone.ERROR) st.copy(stats = st.detail) else st
    }

    private fun linkHeadline(): MenuState = when (link.state) {
        UsbLink.State.NO_ADAPTER -> MenuState(Tone.IDLE, "No adapter", "Plug the Wi-Fi adapter into the phone's USB port.")
        UsbLink.State.PERMISSION -> MenuState(Tone.BUSY, "Allow USB access", "Android is asking whether Kepyur may use the adapter.")
        UsbLink.State.DENIED -> MenuState(Tone.WARN, "Permission needed", "Kepyur cannot use the adapter without it.", "Ask again")
        UsbLink.State.SWITCHING -> MenuState(Tone.BUSY, "Switching to USB 3", "The adapter restarts itself. This takes a few seconds.")
        UsbLink.State.OPENING -> MenuState(Tone.BUSY, "Starting the radio", link.message.ifEmpty { "About five seconds." })
        UsbLink.State.WAITING -> MenuState(Tone.WARN, "Trying again", link.message, "Try now")
        UsbLink.State.FAILED -> MenuState(Tone.ERROR, "Adapter problem", link.message, "Try again")
        UsbLink.State.USB2 -> MenuState(Tone.WARN, "USB 2 link",
            "Video needs USB 3. Plug the adapter straight into the phone with a USB 3 (OTG) adapter or cable.", "Try again")
        UsbLink.State.RUNNING -> MenuState()
    }

    private fun radioState(): MenuState {
        // a number the native side could not print as JSON (nan, inf) must not take the app down with it
        val s = try {
            JSONObject(engine.status())
        } catch (e: org.json.JSONException) {
            if (!statusBroken) engine.log("status: unreadable ($e)")
            statusBroken = true
            return MenuState(Tone.BUSY, "Working", radioReady = !testRunning, found = found)
        }
        statusBroken = false
        link.check(s)
        if (link.state != UsbLink.State.RUNNING) return linkState()
        val was = mode
        mode = s.optString("mode")
        val radioMode = mode   // what the radio says it is doing; `mode` may move on below (the end of a stream test tunes)
        val v = s.optJSONObject("video")
        val locked = v?.optBoolean("locked") ?: false
        val watchHz = s.num("watch_hz")

        // The radio remembers every signal it has ever seen; what the menu shows as found is what the last scan
        // saw (a transmitter that has since landed is not "found", and its old frequency is no guide to tuning).
        val sinceScan = android.os.SystemClock.elapsedRealtime() - scanStartedAt
        found = if (scanStartedAt == 0L) emptyList() else buildList {
            val arr = s.optJSONArray("pilots") ?: return@buildList
            for (i in 0 until arr.length()) {
                val p = arr.optJSONObject(i) ?: continue
                val mhz = p.num("mhz")
                val seenInThisScan = p.optLong("age_s") * 1000 <= sinceScan + 1500
                if (mhz > 0 && seenInThisScan) add(Found(mhz, p.optString("name"), p.num("snr").toFloat(), p.optString("std")))
            }
        }
        // a scan has just ended: it left the radio on the channel it judged best, and the picker follows
        if (was == "scan" && mode != "scan") {
            noVideoFound = found.isEmpty()
            if (mode == "watch") adoptChannel(watchHz / 1e6)
        }
        if (testRunning && mode == "idle") {
            s.optJSONObject("test")?.let { if (it.optBoolean("done")) { testResults.add(it.toString()); nextTest() } }
        }
        if (mode == "watch" && locked) {
            matchDisplayTo(v?.optString("std") ?: "")
            noVideoFound = false   // there is a picture after all: what the scan missed no longer matters
        }

        // Is the phone keeping up? A block lost in the queue behind the USB link or before the decoder, or a thread
        // with no time to spare, says no at once. A block the adapter's stream itself gave up on (collected too
        // late) does not: any phone stalls for a moment now and then, a few times a minute. LATE_BLOCKS of them
        // seen within LATE_WINDOW_MS do (a block counts from the refresh that first shows it, up to a poll late).
        // Only those of a watch that this screen has been following count: a rise between two refreshes that both
        // found the radio watching. The ones a scan or a stream test left behind (up to its last moments, after
        // the last refresh that saw it), or that came in while nothing was refreshed (the app in the background:
        // the adapter streams on for a while), say nothing about the picture now: the count is then only taken
        // note of. So is whatever comes in before the first refresh of a watch (up to a poll's length, the
        // retune into it included): a phone that is not keeping up goes on losing blocks after that.
        val now = android.os.SystemClock.elapsedRealtime()
        val late = s.optLong("stream_dropped")
        val queued = s.optLong("dropped") + s.optLong("decode_dropped")
        val dropped = late + queued   // lost anywhere on the way, for the figures on the Signal tab
        val load = maxOf(s.num("dsp_load"), s.num("decode_load"))
        val followed = radioMode == "watch" && lastLateWatching && late >= lastLate && now - lastLateAt <= LATE_GAP_MS
        if (!followed) lateMarks.clear()
        else if (late > lastLate) lateMarks.addLast(now to late - lastLate)
        while (lateMarks.isNotEmpty() && now - lateMarks.first().first > LATE_WINDOW_MS) lateMarks.removeFirst()
        val lateBurst = followed && late > lastLate && lateMarks.sumOf { it.second } >= LATE_BLOCKS
        lastLate = late
        lastLateAt = now
        lastLateWatching = radioMode == "watch"
        if (mode == "watch" && (queued > lastDropped || load > 0.95 || lateBurst)) overloadedUntil = now + 5000
        lastDropped = queued
        val overloaded = mode == "watch" && now < overloadedUntil

        val name = Channels.nearest(watchHz / 1e6, settings.band, Channels.SAME_CHANNEL_MHZ)?.name
        val where = (if (name != null) "$name · " else "") + "${(watchHz / 1e6).roundToInt()} MHz"   // finite: num()
        val stats = buildString {
            append(if (s.optBoolean("usb3")) "USB 3" else "USB 2")
            val msps = s.num("watch_msps")
            if (msps > 0) append(" · %.0f MS/s".format(msps))
            if (s.optBoolean("fallback")) append(" (40 kept dropping out)")
            append(" · processing %.0f%%".format(load * 100))
            append(" · $dropped dropped · ${s.optLong("restarts")} restarts")
            s.optJSONObject("antennas")?.let { a ->
                if (a.optBoolean("combine")) append("\nAntenna A %.0f dB · B %.0f dB · together %.0f dB".format(
                    a.num("snr_a"), a.num("snr_b"), a.num("snr_c")))
            }
        }
        val base = MenuState(radioReady = !testRunning, scanning = mode == "scan", scanStep = s.optInt("scan_step"),
            scanSteps = s.optInt("scan_steps"), found = found, scanFoundNothing = noVideoFound, stats = stats)
        return when (mode) {
            "scan" -> base.copy(tone = Tone.BUSY, headline = "Scanning", detail = "Looking for video on every channel.")
            "test" -> {
                val t = s.optJSONObject("test")
                base.copy(tone = Tone.BUSY, headline = "USB test", detail = t?.let {
                    "%.0f MS/s · %.0f s\n%d of %d blocks · %d gaps · %d restarts".format(it.num("rate_msps"), it.num("seconds"),
                        it.optLong("banks"), it.optLong("expected"), it.optLong("seq_gaps"), it.optLong("restarts"))
                } ?: "Starting…")
            }
            "watch" -> when {
                overloaded -> OVERLOAD_ADVICE.let { advice ->   // in full on the Signal tab: the rail may cut it short
                    base.copy(tone = Tone.WARN, headline = where, detail = advice, stats = "$advice\n$stats")
                }
                locked -> base.copy(tone = Tone.LIVE, headline = where, detail = buildString {
                    append(v?.optString("std") ?: "")
                    if (v?.optBoolean("colour") == false) append(" · black and white")
                    append(" · %.0f fps".format(v?.num("fps") ?: 0.0))
                    if (s.has("snr_db")) append(" · %.0f dB".format(s.num("snr_db")))
                })
                noVideoFound -> base.copy(tone = Tone.BUSY, headline = where,
                    detail = "The scan found no video. Still looking on this channel…")
                else -> base.copy(tone = Tone.BUSY, headline = where, detail = "Looking for a picture…")
            }
            else -> base.copy(tone = Tone.IDLE, headline = "Ready",
                detail = if (noVideoFound) "The scan found no video. Pick a channel, or scan again." else "Pick a channel, or scan.")
        }
    }

    /** The radio is on [mhz] by its own choice (after a scan): the picker and the saved channel follow. */
    private fun adoptChannel(mhz: Double) {
        if (!mhz.isFinite() || mhz <= 0) return
        // a transmitter further than that from a channel is not on it: it is kept by its own frequency
        val ch = Channels.nearest(mhz, settings.band, Channels.SAME_CHANNEL_MHZ)
        val to = ch?.mhz ?: mhz.roundToInt()
        if (to == settings.channelMhz && (ch == null || ch.band == settings.band)) return
        settings.channelMhz = to
        if (ch != null) settings.band = ch.band
        settings.save()
        menu.syncChannel()
    }

    /** Ask for a display mode that fits the camera's field rate, so frames do not beat against the refresh. */
    private fun matchDisplayTo(std: String) {
        if (Build.VERSION.SDK_INT < 30 || std == frameRateFor) return
        val rate = when (std) { "PAL" -> 50f; "NTSC" -> 59.94f; else -> return }
        val surface = video.holder.surface ?: return
        if (!surface.isValid) return
        frameRateFor = std
        try {
            surface.setFrameRate(rate, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
        } catch (e: Exception) {
            engine.log("setFrameRate: $e")
        }
    }

    // ---------------------------------------------------------------------------------------- UI
    private fun dp(v: Float) = v * resources.displayMetrics.density

    @SuppressLint("ClickableViewAccessibility")
    private fun buildUi() {
        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        video = SurfaceView(this)
        video.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) {
                frameRateFor = ""
                surfaceOwner = surfaceToken
                engine.setSurface(h.surface)
            }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) = engine.redraw()
            override fun surfaceDestroyed(h: SurfaceHolder) {
                // the native screen is one for the process: an earlier activity, going away late, must not take
                // the Surface of the one that has replaced it
                if (surfaceOwner !== surfaceToken) return
                surfaceOwner = null
                engine.setSurface(null)
            }
        })
        root.addView(video, FrameLayout.LayoutParams(-1, -1))

        hint = MenuHint(this).apply {
            alpha = 0f
            visibility = View.GONE
            setOnClickListener { openMenu() }
        }
        root.addView(hint, FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL)
            .apply { bottomMargin = dp(30f).roundToInt() })

        menu = MenuView(this, settings, this)
        root.addView(menu, FrameLayout.LayoutParams(-1, -1))

        // a tap on the picture: the MENU button, for three seconds
        var downX = 0f
        var downY = 0f
        root.setOnTouchListener { _, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { downX = e.x; downY = e.y }
                MotionEvent.ACTION_UP -> if (!menuOpen && abs(e.x - downX) < dp(16f) && abs(e.y - downY) < dp(16f)) showHint()
            }
            true
        }
        setContentView(root)
    }

    private val hideHint = Runnable {
        hint.animate().alpha(0f).setDuration(450).withEndAction { hint.visibility = View.GONE }.start()
    }

    private fun showHint() {
        ui.removeCallbacks(hideHint)
        hint.animate().cancel()
        hint.visibility = View.VISIBLE
        hint.animate().alpha(1f).setDuration(140).start()
        ui.postDelayed(hideHint, HINT_MS)
    }

    private fun openMenu() {
        if (menuOpen) return
        menuOpen = true
        menuAuto = false
        ui.removeCallbacks(hideHint)
        hint.animate().cancel()
        hint.alpha = 0f
        hint.visibility = View.GONE
        engine.setBackdrop(true, false)
        refresh()
        menu.animate().cancel()
        menu.visibility = View.VISIBLE
        menu.alpha = 0f
        menu.translationY = dp(10f)
        menu.animate().alpha(1f).translationY(0f).setDuration(240).setInterpolator(DecelerateInterpolator(1.5f)).withEndAction(null).start()
    }

    private fun closeMenu() {
        if (!menuOpen) return
        menuOpen = false
        menuAuto = false
        menu.endPeek()
        engine.setBackdrop(false, false)
        menu.animate().cancel()
        menu.animate().alpha(0f).translationY(dp(10f)).setDuration(200).setInterpolator(DecelerateInterpolator(1.5f))
            .withEndAction { menu.visibility = View.GONE }.start()
    }

    /**
     * The whole display, bars hidden, and laid out under a camera cutout too: the picture and the MENU button
     * are centred on the screen itself (goggles need the two pictures either side of its middle), and the menu
     * keeps clear of the cutout by its own padding.
     */
    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {   // Android 10: the flags that came before the insets controller
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        }
    }

    companion object {
        const val HINT_MS = 3000L
        const val BACKGROUND_CLOSE_MS = 30000L
        const val LATE_BLOCKS = 3
        const val LATE_WINDOW_MS = 5000L
        /** No refresh for longer than this (the poll runs every 250 ms): the late blocks counted meanwhile are not this watch's. */
        const val LATE_GAP_MS = 2000L
        const val OVERLOAD_ADVICE =
            "The phone is not keeping up. Turn off Clean OSD edges, Colour comb or Diversity, or use sample rate 20."

        /** The activity whose SurfaceView the native screen is drawing on, by its token (main thread only). */
        private var surfaceOwner: Any? = null
        /** Debug ("soak" extra): after every open, a 40 MS/s stream test of [SOAK_S] instead of a watch or scan. */
        private var soak = false
        const val SOAK_S = 600.0
    }
}
