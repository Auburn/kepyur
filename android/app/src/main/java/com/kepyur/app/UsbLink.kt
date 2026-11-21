package com.kepyur.app

import android.annotation.SuppressLint
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import org.json.JSONObject
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * The adapter's whole life: found, permitted, switched to USB 3, opened, watched over while it runs, and
 * brought back when anything goes wrong. Nothing here waits for the user: a failed start is tried again by
 * itself, a radio that stops delivering is closed and reopened, and every wait has an end.
 *
 * All of it runs on the main thread except the native calls that block (open, close, the USB 3 switch), which
 * go through one worker thread, in order.
 */
class UsbLink(context: Context, private val engine: Engine, private val events: Events) {
    private val ctx: Context = context.applicationContext   // receivers and the USB service outlive no activity
    interface Events {
        /** [state] or [message] changed. */
        fun onLinkChanged()
        /** The radio has just opened: apply the settings and tune. Also after every automatic reopen. */
        fun onRadioUp()
    }

    enum class State {
        NO_ADAPTER,   // nothing plugged in
        PERMISSION,   // Android is asking the user
        DENIED,       // the user said no
        SWITCHING,    // the adapter is re-enumerating as a USB 3 device
        OPENING,      // chip bring-up (about five seconds)
        RUNNING,
        USB2,         // open, but on a link too slow for video
        WAITING,      // something failed; trying again shortly ([message] says what)
        FAILED,       // something failed that trying again by itself will not fix ([message])
    }

    var state = State.NO_ADAPTER
        private set
    var message = ""
        private set

    private val ui = Handler(Looper.getMainLooper())
    private val usb = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
    private val permAction = "com.kepyur.app.USB_PERMISSION"
    private var conn: UsbDeviceConnection? = null
    var started = false
        private set

    /** Bumped whenever the adapter in hand is given up: what an open in flight finds when it lands tells it to stand down. */
    private var generation = 0
    /** This link, to the process-wide [holder]: a token, so that nothing static holds on to an activity. */
    private val me = Any()
    private var failures = 0
    private var runningSince = 0L
    /** The device a permission request is out for (no second request, and no second dialog, while it is). */
    private var askedFor: String? = null
    private var switchRequests = 0
    private var switchDeadline = 0L
    /** Shown while the adapter is opened again because its USB data path wedged; "" otherwise. */
    private var reopenNote = ""
    /** When the data path wedged, over the last [WEDGE_WINDOW_MS] (elapsedRealtime). */
    private val wedges = ArrayDeque<Long>()
    /**
     * The adapter is being opened again, at once, because its USB data path wedged ([reopenAfterWedge]): a gap of a
     * few seconds that fixes itself, not trouble to open the menu for.
     */
    val reopeningAfterWedge get() = reopenNote.isNotEmpty()
    // the bank counter of the last status, and when it last moved (a stream that has silently stopped)
    private var lastBanks = -1L
    private var banksMovedAt = 0L
    private var checkedAt = 0L

    // Before Android 13 a receiver registered in code is open to any sender, whatever is asked for. So these
    // broadcasts are taken as a nudge only: UsbManager itself is asked what is plugged in and what is permitted.
    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    fun start() {
        if (started) return
        started = true
        val f = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= 33) {
            ctx.registerReceiver(usbReceiver, f, Context.RECEIVER_EXPORTED)
            ctx.registerReceiver(permReceiver, IntentFilter(permAction), Context.RECEIVER_NOT_EXPORTED)
        } else {
            ctx.registerReceiver(usbReceiver, f)
            ctx.registerReceiver(permReceiver, IntentFilter(permAction))
        }
        connect()
    }

    fun stop() {
        if (!started) return
        started = false
        ui.removeCallbacksAndMessages(null)
        ctx.unregisterReceiver(usbReceiver)
        ctx.unregisterReceiver(permReceiver)
        giveUpAdapter()
        // quietly: nobody is listening any more, and start() begins from nothing
        state = State.NO_ADAPTER
        message = ""
        askedFor = null
        switchDeadline = 0
        failures = 0
        reopenNote = ""
        wedges.clear()
    }

    /** The user asked for another go (a tap on the status). */
    fun retryNow() {
        when (state) {
            State.DENIED, State.WAITING, State.FAILED, State.USB2, State.NO_ADAPTER -> {
                ui.removeCallbacks(retry)
                if (state == State.USB2) {   // open, on a slow link: close it and go through the switch again
                    giveUpAdapter()
                    state = State.NO_ADAPTER
                }
                askedFor = null
                switchRequests = 0
                connect(force = true)
            }
            else -> {}
        }
    }

    /** Called from the app's own launch intent when Android started it for an attached adapter. */
    fun onAttachedIntent() {
        if (state != State.RUNNING && state != State.OPENING) connect(force = true)
    }

    // ---------------------------------------------------------------------------------------- connecting
    private fun ours(d: UsbDevice) = // T4U v3, T3U, T3U Plus; Realtek's own 8812BU id
        (d.vendorId == 0x2357 && d.productId in setOf(0x0115, 0x012d, 0x0138)) ||
            (d.vendorId == 0x0bda && d.productId == 0xb812)

    private fun set(s: State, msg: String = "") {
        if (s == state && msg == message) return
        state = s
        message = msg
        events.onLinkChanged()
    }

    private fun connect(force: Boolean = false) {
        if (!started) return
        if (state == State.OPENING || state == State.RUNNING || state == State.USB2) return
        if (state == State.DENIED && !force) return
        ui.removeCallbacks(retry)
        val d = usb.deviceList.values.firstOrNull { ours(it) }
        if (d == null) {
            // gone because it is coming back as a USB 3 device? then keep saying so until the deadline
            if (SystemClock.elapsedRealtime() < switchDeadline) awaitSwitch() else set(State.NO_ADAPTER)
            return
        }
        // it is back (or was never away): from here on, its going away is an unplugging like any other
        switchDeadline = 0
        if (!usb.hasPermission(d)) {
            // already asked about this very device and still waiting for the answer (the attach broadcast and
            // the USB 3 switch watch both come through here)
            if (state == State.PERMISSION && askedFor == d.deviceName) return
            askedFor = d.deviceName
            set(State.PERMISSION)
            val pi = PendingIntent.getBroadcast(
                ctx, 0, Intent(permAction).setPackage(ctx.packageName),
                PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            )
            usb.requestPermission(d, pi)
            return
        }
        askedFor = null
        val c = usb.openDevice(d)
        if (c == null) {
            failLater("Android would not open the adapter. Another app may be using it.")
            return
        }
        conn = c
        val fd = c.fileDescriptor
        val gen = ++generation
        engine.log("adapter ${d.deviceName} ${"%04x:%04x".format(d.vendorId, d.productId)} fd=$fd")
        set(State.OPENING, reopenNote)
        // bring-up cannot be interrupted from here, but pulling the plug ends it: say so if it drags on
        ui.postDelayed({
            if (gen == generation && state == State.OPENING)
                set(State.OPENING, "This is taking longer than it should. Unplug the adapter and plug it in again.")
        }, SLOW_OPEN_MS)
        val trySwitch = switchRequests < MAX_SWITCH_REQUESTS
        worker.execute {
            val sw = if (trySwitch) engine.usb3Switch(fd) else "skipped"
            if (sw.startsWith("requested")) {
                // the adapter drops off the bus and comes back as SuperSpeed: a new device, a new permission
                ui.post {
                    if (gen != generation) return@post
                    switchRequests++
                    generation++
                    c.close()
                    conn = null
                    switchDeadline = SystemClock.elapsedRealtime() + SWITCH_WAIT_MS
                    awaitSwitch()
                }
                return@execute
            }
            linkReport(c)
            holder = me   // from here on the native radio is ours to close
            val err = engine.open(fd)
            val fast = err.isEmpty() && engine.superspeed()
            ui.post {
                if (gen != generation) return@post   // unplugged meanwhile: the close queued behind us tidies up
                if (err.isNotEmpty() || !fast) reopenNote = ""   // trouble of another kind: said as such
                when {
                    err.isNotEmpty() -> failLater("The radio did not start: $err")
                    !fast -> set(State.USB2)
                    else -> {
                        reopenNote = ""
                        // failures is not forgotten yet: a radio that opens and then never streams is still failing
                        lastBanks = -1
                        banksMovedAt = SystemClock.elapsedRealtime()
                        runningSince = banksMovedAt
                        checkedAt = banksMovedAt
                        set(State.RUNNING)
                        if (state == State.RUNNING) events.onRadioUp()   // a listener may already have moved on
                    }
                }
            }
        }
    }

    /**
     * For the log, on the worker before the radio opens: the USB 3 link's power management as the adapter reports it
     * (GET_STATUS: whether the host let it start U1 / U2 itself; the BOS descriptor: its exit latencies). A phone that
     * puts the link to sleep is one thing to rule out when the stream keeps failing.
     */
    private fun linkReport(c: UsbDeviceConnection) {
        try {
            fun status(): String {
                val b = ByteArray(2)
                val n = c.controlTransfer(0x80, 0 /* GET_STATUS */, 0, 0, b, 2, 500)
                if (n != 2) return "GET_STATUS failed ($n)"
                val s = b[0].toInt() and 0xff
                return "U1 ${if (s and 4 != 0) "on" else "off"}, U2 ${if (s and 8 != 0) "on" else "off"}, " +
                    "LTM ${if (s and 16 != 0) "on" else "off"} (status 0x%02x)".format(s)
            }
            val bos = ByteArray(64)
            val n = c.controlTransfer(0x80, 6 /* GET_DESCRIPTOR */, 0x0f shl 8 /* BOS */, 0, bos, bos.size, 500)
            var lat = "no BOS ($n)"
            var i = 5
            while (n >= 5 && i + 2 < n) {
                val len = bos[i].toInt() and 0xff
                if (len < 3) break
                // SuperSpeed USB device capability: bU1DevExitLat at +7, wU2DevExitLat at +8
                if ((bos[i + 2].toInt() and 0xff) == 3 && i + 9 < n)
                    lat = "exit latency U1 ${bos[i + 7].toInt() and 0xff} us, U2 " +
                        "${(bos[i + 8].toInt() and 0xff) or ((bos[i + 9].toInt() and 0xff) shl 8)} us"
                i += len
            }
            engine.log("usb link: ${status()}; $lat")
        } catch (e: Exception) {
            engine.log("usb link: report failed: $e")
        }
    }

    /** Waiting for the adapter to come back as a USB 3 device, with the watch that ends the wait running. */
    private fun awaitSwitch() {
        set(State.SWITCHING)
        ui.removeCallbacks(switchWatch)
        ui.postDelayed(switchWatch, 1000)
    }

    /** While the adapter re-enumerates: open it as soon as it is back; say so if it never comes. */
    private val switchWatch = object : Runnable {
        override fun run() {
            if (state != State.SWITCHING) return
            if (usb.deviceList.values.any { ours(it) }) {
                connect()
                if (state != State.SWITCHING) return
            }
            if (SystemClock.elapsedRealtime() >= switchDeadline) {
                switchDeadline = 0
                set(State.FAILED, "The adapter did not come back after switching to USB 3. Unplug it and plug it in again.")
                return
            }
            ui.postDelayed(this, 1000)
        }
    }

    private val retry = Runnable { connect(force = true) }

    /**
     * Close what there is, say what went wrong, and try again shortly: soon the first time, then every
     * [RETRY_MS], and less often once it keeps failing (it never gives up: the cause may be a loose plug).
     */
    private fun failLater(msg: String) {
        engine.log("link: $msg (failure ${failures + 1})")
        reopenNote = ""
        giveUpAdapter()
        failures++
        val persistent = failures >= PERSISTENT_FAILURES
        set(State.WAITING, if (persistent) "$msg If this goes on, unplug the adapter and plug it in again." else msg)
        ui.removeCallbacks(retry)
        ui.postDelayed(retry, if (failures <= 1) FIRST_RETRY_MS else if (persistent) SLOW_RETRY_MS else RETRY_MS)
    }

    /**
     * Stop using the adapter in hand. The connection closes after the native side has let go of its fd, and the
     * native radio is closed only if it is this link's: an earlier activity's link, stopping late, must not
     * close the radio a newer one has opened since.
     */
    private fun giveUpAdapter() {
        generation++
        val c = conn
        conn = null
        worker.execute {
            if (holder === me) {
                engine.close()
                holder = null
            }
            c?.close()
        }
    }

    // ---------------------------------------------------------------------------------------- watching over it
    /**
     * Once per status poll while [State.RUNNING]: is the radio still alive? A radio whose receiver has ended
     * ("dead", or "lost": the adapter is gone), one that has been closed underneath us, or one that says it is
     * streaming while no samples have arrived for [STALL_MS], is closed and reopened; [Events.onRadioUp] then
     * tunes the channel again.
     *
     * The receiver's other states are not verdicts. It reads "closed" from the moment it is opened until its
     * own thread first runs, and "error" for a fifth of a second before it tries a failed retune again: a poll
     * that lands there must not take a healthy radio down. If it does stay there, no samples come, and the
     * stall rule ends it.
     */
    fun check(status: JSONObject) {
        if (state != State.RUNNING) return
        val now = SystemClock.elapsedRealtime()
        val banks = status.optLong("banks", -1)
        // polling paused for a while (the app was in the background): the clock starts again
        if (now - checkedAt > 1500) banksMovedAt = now
        checkedAt = now
        if (banks != lastBanks) {
            lastBanks = banks
            banksMovedAt = now
        }
        if (status.optBoolean("wedged")) {
            reopenAfterWedge(status)
            return
        }
        val rx = status.optString("rx_state")
        val why = when {
            status.optBoolean("dead") || rx == "lost" ->
                status.optString("rx_error").ifEmpty { status.optString("last_fault") }.ifEmpty { "the adapter stopped answering" }
            !status.optBoolean("open", true) -> "it was closed"   // under us: nothing will come
            now - banksMovedAt > STALL_MS -> "no samples for ${STALL_MS / 1000} seconds"
            else -> {
                if (now - runningSince > HEALTHY_MS) failures = 0   // it has run well for a while: a clean slate
                return
            }
        }
        failLater("The radio stopped: $why.")
    }

    /**
     * The chip's USB data path has wedged (wifi-sdr's C2H wedge, which the radio reports as "wedged"): on some USB 3
     * ports (a Pixel 8 Pro's, for one) it happens every few seconds to minutes, and only opening the adapter again
     * brings it back. That takes about three seconds, so it is done at once: not as a failure (no wait, nothing
     * counted towards slower retries), and quietly ([reopeningAfterWedge]). When it keeps happening, [message] says
     * that this phone and this adapter do not get on.
     */
    private fun reopenAfterWedge(status: JSONObject) {
        val now = SystemClock.elapsedRealtime()
        wedges.addLast(now)
        while (now - wedges.first() > WEDGE_WINDOW_MS) wedges.removeFirst()
        val n = wedges.size
        val minutes = WEDGE_WINDOW_MS / 60000
        engine.log("link: the USB data path wedged ($n in $minutes min): opening the adapter again at once; " +
            status.optString("rx_error"))
        engine.holdPicture(HOLD_PICTURE_MS)   // the last picture stays, dimmed, until the reopened radio's first frame
        giveUpAdapter()
        reopenNote = if (n >= WEDGE_ADVICE_COUNT)
            "The adapter's USB link locked up $n times in $minutes minutes: starting it again. This phone's USB 3 " +
                "port and this adapter do not get on; another phone or adapter may hold up better."
        else "The adapter's USB link locked up: starting it again."
        state = State.WAITING   // quietly: connect() says OPENING (with the note) straight away
        connect(force = true)
    }

    // ---------------------------------------------------------------------------------------- broadcasts
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val d: UsbDevice? = if (Build.VERSION.SDK_INT >= 33)
                i.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            else @Suppress("DEPRECATION") i.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            if (d == null || !ours(d)) return
            when (i.action) {
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    // really gone? (a detach nobody sent would otherwise take the adapter away for good; if this
                    // one is real and the list is merely behind, the radio dying brings us back here anyway)
                    if (usb.deviceList.values.any { it.deviceName == d.deviceName }) return
                    val switching = SystemClock.elapsedRealtime() < switchDeadline
                    engine.log("adapter detached${if (switching) " (USB 3 switch)" else ""}")
                    ui.removeCallbacks(retry)
                    giveUpAdapter()
                    failures = 0
                    askedFor = null
                    reopenNote = ""
                    if (switching) awaitSwitch() else { switchRequests = 0; set(State.NO_ADAPTER) }
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    engine.log("adapter attached")
                    if (state != State.RUNNING && state != State.OPENING) connect(force = true)
                }
            }
        }
    }

    private val permReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val ok = i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            engine.log("usb permission ${if (ok) "granted" else "denied"}")
            if (state != State.PERMISSION) return
            if (ok) connect(force = true) else set(State.DENIED)
        }
    }

    companion object {
        /**
         * One worker for the process, as the native radio is one for the process: every open and close runs in
         * the order it was asked for, whichever activity instance asked.
         */
        private val worker: ExecutorService =
            Executors.newSingleThreadExecutor { r -> Thread(r, "kepyur-usb").apply { isDaemon = true } }
        /** The token ([me]) of the link whose radio the native side has open, if any (touched on the worker only). */
        private var holder: Any? = null

        const val FIRST_RETRY_MS = 2500L
        const val RETRY_MS = 5000L
        const val SLOW_RETRY_MS = 10000L
        const val PERSISTENT_FAILURES = 4   // in a row, with no healthy spell between
        const val HEALTHY_MS = 10000L
        const val SWITCH_WAIT_MS = 14000L   // re-enumeration takes "well over 4 s" on some controllers
        const val STALL_MS = 4000L
        const val SLOW_OPEN_MS = 25000L
        const val MAX_SWITCH_REQUESTS = 2   // a link without USB 3 lines would otherwise bounce for ever
        const val WEDGE_WINDOW_MS = 120000L
        const val WEDGE_ADVICE_COUNT = 3    // wedges within WEDGE_WINDOW_MS before the note says what to try
        const val HOLD_PICTURE_MS = 6000    // a reopen takes ~3 s; the picture goes black if it takes longer than this
    }
}
