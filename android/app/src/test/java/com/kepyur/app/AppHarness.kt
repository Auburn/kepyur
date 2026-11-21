package com.kepyur.app

import android.app.Application
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbConfiguration
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Looper
import android.os.SystemClock
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import org.junit.After
import org.junit.Assert.fail
import org.junit.Before
import org.robolectric.Robolectric
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.android.controller.ActivityController
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import org.robolectric.shadow.api.Shadow
import org.robolectric.shadows.ShadowUsbManager
import java.time.Duration
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ExecutorService
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

/**
 * Robolectric's UsbManager shadow, plus a record of permission requests (4.17 has no shadow for
 * requestPermission: the real one would call a null system service). The request's PendingIntent is kept so a
 * test can answer it the way the system does.
 */
@Implements(UsbManager::class)
class RecordingUsbManager : ShadowUsbManager() {
    @Implementation
    protected fun requestPermission(device: UsbDevice, pi: PendingIntent) {
        requests += device to pi
    }

    companion object {
        val requests = CopyOnWriteArrayList<Pair<UsbDevice, PendingIntent>>()
    }
}

/**
 * What every scenario needs: the fake native side in [Engine.standIn], clean settings and USB state, the activity,
 * and ways to let time pass. UsbLink runs its blocking engine calls on its own thread; [settle] waits for that
 * thread and the main looper together until neither has anything left to do now.
 */
abstract class AppHarness {
    val fake = FakeEngine()
    val app: Application get() = RuntimeEnvironment.getApplication()
    val usb: UsbManager get() = app.getSystemService(Context.USB_SERVICE) as UsbManager
    val usbShadow: RecordingUsbManager get() = Shadow.extract(usb)
    val looper get() = shadowOf(Looper.getMainLooper())

    lateinit var controller: ActivityController<MainActivity>
    val activity: MainActivity get() = controller.get()
    private val links = ArrayList<UsbLink>()

    @Before
    fun harnessUp() {
        Engine.standIn = fake
        app.getSharedPreferences("settings", Context.MODE_PRIVATE).edit().clear().commit()
        ShadowUsbManager.reset()
        RecordingUsbManager.requests.clear()
    }

    @After
    fun harnessDown() {
        fake.release()
        // the worker is one for the process: nothing of this test may still be in it when the next one starts
        runCatching { drainWorkers(5000) }
        Engine.standIn = null
    }

    // ------------------------------------------------------------------------------------------ the app
    /**
     * Create, start and resume the activity. The fake holds the worker's first engine call until the activity is
     * resumed, as on a device (where onCreate, onStart and onResume run in one main-thread message, so nothing the
     * worker posts can land between them; Robolectric idles the looper after each step). [holdOpen]: keep holding
     * (the test releases [FakeEngine.release]).
     */
    fun launch(intent: Intent? = null, holdOpen: Boolean = false): MainActivity {
        fake.hold()
        controller = Robolectric.buildActivity(MainActivity::class.java, intent).setup()
        links += link
        if (!holdOpen) {
            fake.release()
            settle()
        } else {
            looper.idle()
        }
        return activity
    }

    val link: UsbLink get() = linkOf(activity)

    fun linkOf(a: MainActivity): UsbLink =
        MainActivity::class.java.getDeclaredField("link").apply { isAccessible = true }.get(a) as UsbLink

    /** UsbLink's worker: one for the process (a static field), whichever link is asked. */
    private fun workerOf(l: UsbLink): ExecutorService =
        UsbLink::class.java.getDeclaredField("worker").apply { isAccessible = true }.get(l) as ExecutorService

    fun saveChannel(mhz: Int, band: Char) {
        val s = Settings(app)
        s.channelMhz = mhz
        s.band = band
        s.save()
    }

    fun savedSettings() = Settings(app)

    // ------------------------------------------------------------------------------------------ time
    /** Wait for every UsbLink worker to run what it has been given so far. */
    private fun drainWorkers(timeoutMs: Long) {
        for (l in links) {
            val w = workerOf(l)
            if (w.isShutdown) continue
            try {
                w.submit {}.get(timeoutMs, TimeUnit.MILLISECONDS)
            } catch (e: TimeoutException) {
                fail("UsbLink's worker is still busy after $timeoutMs ms (a held FakeEngine.open?). " +
                    "state=${l.state}, calls=${fake.radioCalls()}")
            }
        }
    }

    /** Run the worker and the main looper until neither has anything to do at the current (fake) time. */
    fun settle(timeoutMs: Long = 5000) {
        val end = System.nanoTime() + timeoutMs * 1_000_000
        while (true) {
            drainWorkers(timeoutMs)
            if (looper.isIdle) return
            looper.idle()
            if (System.nanoTime() > end) fail("settle: still busy after $timeoutMs ms; calls=${fake.radioCalls()}")
        }
    }

    /** Poll [cond], idling the looper each time; fail with [what] after [timeoutMs] of real time. */
    fun waitUntil(what: String, timeoutMs: Long = 5000, cond: () -> Boolean) {
        val end = System.nanoTime() + timeoutMs * 1_000_000
        while (true) {
            looper.idle()
            if (cond()) return
            if (System.nanoTime() > end)
                fail("Timed out after $timeoutMs ms waiting for: $what. state=${runCatching { link.state }.getOrNull()}, " +
                    "calls=${fake.radioCalls()}")
            Thread.sleep(1)
        }
    }

    /**
     * Let [ms] of fake time pass, in small steps, settling the worker after each (so a retry's open completes when
     * due). Runs to an absolute time: settling can itself move the clock on by a few ms (Robolectric's frame
     * callbacks), so counting steps would come up short.
     */
    fun advance(ms: Long, step: Long = 50) {
        val target = SystemClock.uptimeMillis() + ms
        while (true) {
            val left = target - SystemClock.uptimeMillis()
            if (left <= 0) return
            looper.idleFor(Duration.ofMillis(minOf(step, left)))
            settle()
        }
    }

    /** Advance until [cond] holds, at most [maxMs] of fake time; returns how long it took. */
    fun advanceUntil(what: String, maxMs: Long, step: Long = 50, cond: () -> Boolean): Long {
        val t0 = SystemClock.elapsedRealtime()
        while (!cond()) {
            if (SystemClock.elapsedRealtime() - t0 > maxMs)
                fail("Not within $maxMs ms: $what. state=${link.state} '${link.message}', calls=${fake.radioCalls()}")
            looper.idleFor(Duration.ofMillis(step))
            settle()
        }
        return SystemClock.elapsedRealtime() - t0
    }

    // ------------------------------------------------------------------------------------------ USB
    private var nextAddress = 2

    /** An adapter the app knows: TP-Link Archer T4U v3 (2357:0115), at a new bus address each time. */
    fun adapter(vid: Int = 0x2357, pid: Int = 0x0115): UsbDevice = newUsbDevice("/dev/bus/usb/001/%03d".format(nextAddress++), vid, pid)

    /** Put [d] on the bus; [broadcast]: and tell the app (ACTION_USB_DEVICE_ATTACHED). */
    fun plug(d: UsbDevice, permission: Boolean = true, broadcast: Boolean = true) {
        usbShadow.addOrUpdateUsbDevice(d, permission)
        if (broadcast) {
            app.sendBroadcast(Intent(UsbManager.ACTION_USB_DEVICE_ATTACHED).putExtra(UsbManager.EXTRA_DEVICE, d))
            settle()
        }
    }

    fun unplug(d: UsbDevice, broadcast: Boolean = true) {
        usbShadow.removeUsbDevice(d)
        if (broadcast) {
            app.sendBroadcast(Intent(UsbManager.ACTION_USB_DEVICE_DETACHED).putExtra(UsbManager.EXTRA_DEVICE, d))
            looper.idle()
        }
    }

    val permissionRequests get() = RecordingUsbManager.requests.toList()

    /** Answer the last permission request as the system does: grant (or not), then fire the app's PendingIntent. */
    fun answerPermission(granted: Boolean) {
        val (d, pi) = permissionRequests.lastOrNull() ?: run { fail("no permission request was made"); return }
        if (granted) usbShadow.addOrUpdateUsbDevice(d, true)
        pi.send(app, 0, Intent().putExtra(UsbManager.EXTRA_DEVICE, d).putExtra(UsbManager.EXTRA_PERMISSION_GRANTED, granted))
        looper.idle()
        settle()
    }

    // ------------------------------------------------------------------------------------------ views
    val root: FrameLayout get() = (activity.findViewById<ViewGroup>(android.R.id.content)).getChildAt(0) as FrameLayout
    val menu: MenuView get() = views().filterIsInstance<MenuView>().single()
    val hint: MenuHint get() = views().filterIsInstance<MenuHint>().single()

    fun views(from: View = activity.window.decorView): List<View> {
        val out = ArrayList<View>()
        fun walk(v: View) {
            out += v
            if (v is ViewGroup) for (i in 0 until v.childCount) walk(v.getChildAt(i))
        }
        walk(from)
        return out
    }

    /** The menu's status, as shown: the headline over the preview, the detail and the action along the foot. */
    private fun shown(v: TextView) = if (v.visibility == View.VISIBLE) v.text.toString() else ""

    val headline: String get() = shown(menu.statusTitle)
    val detail: String get() = shown(menu.statusDetail)
    val action: String get() = shown(menu.statusAction)

    /** A tap on the status, as the user does to retry. */
    fun tapStatus() {
        menu.statusRow.performClick()
        settle()
    }

    fun pill(text: String): PillButton = views(menu).filterIsInstance<PillButton>().first { it.text.toString() == text }

    /** The picture, small, at the foot of the rail: tapping it closes the menu. */
    fun preview(): PreviewTile = views(menu).filterIsInstance<PreviewTile>().single()

    fun tab(name: String): TextView = views(menu).filterIsInstance<TextView>().first { it.text.toString() == name && it.isClickable }

    fun showTab(name: String) {
        tab(name).performClick()
        looper.idleFor(Duration.ofMillis(300))
        layOut()
    }

    /** The control on the settings row titled [title]. */
    fun control(title: String): View = views(menu).filterIsInstance<SettingRow>().first { row ->
        views(row).any { it is TextView && it.text.toString() == title }
    }.control

    fun tile(name: String): ChannelTile = views(menu).filterIsInstance<ChannelTile>().first { it.channel?.name == name }

    /** Click [v] the way a finger would only if it can be pressed. */
    fun click(v: View) {
        if (!v.isEnabled) fail("$v is disabled")
        if (!v.isShown) fail("$v is not shown")
        v.performClick()
        settle()
    }

    fun layOut(w: Int = 2400, h: Int = 1080) {
        val r = activity.window.decorView
        r.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY))
        r.layout(0, 0, w, h)
    }

    fun touch(v: View, action: Int, x: Float, y: Float, downTime: Long = SystemClock.uptimeMillis()): Boolean {
        val e = MotionEvent.obtain(downTime, SystemClock.uptimeMillis(), action, x, y, 0)
        try {
            return v.dispatchTouchEvent(e)
        } finally {
            e.recycle()
        }
    }

    /** A finger down and up at ([x], [y]) on [v]. */
    fun tap(v: View, x: Float = v.width / 2f, y: Float = v.height / 2f) {
        val t = SystemClock.uptimeMillis()
        touch(v, MotionEvent.ACTION_DOWN, x, y, t)
        touch(v, MotionEvent.ACTION_UP, x, y, t)
        settle()
    }

    /** Pick segment [index] of [count] on a laid-out [Segmented]. */
    fun pick(seg: Segmented, index: Int, count: Int) {
        if (seg.width == 0) fail("the Segmented control is not laid out (is its tab shown?)")
        tap(seg, seg.width * (index + 0.5f) / count)
    }

    /** Drag a laid-out [Slider] (range [min]..[max]) to [value] and lift. */
    fun slide(s: Slider, min: Float, max: Float, value: Float) {
        if (s.width == 0) fail("the Slider is not laid out (is its tab shown?)")
        val edge = s.dp(12f)
        val x = edge + (value - min) / (max - min) * (s.width - 2 * edge)
        val t = SystemClock.uptimeMillis()
        touch(s, MotionEvent.ACTION_DOWN, x, s.height / 2f, t)
        settle()
        touch(s, MotionEvent.ACTION_UP, x, s.height / 2f, t)
        settle()
    }

    companion object {
        const val PHONE = "w914dp-h411dp-land-420dpi"

        /** UsbDevice's constructor is hidden; its (hidden) Builder is the way the system makes one. */
        fun newUsbDevice(name: String, vid: Int, pid: Int): UsbDevice {
            val builder = Class.forName("android.hardware.usb.UsbDevice\$Builder")
            val b = builder.getConstructor(
                String::class.java, Int::class.java, Int::class.java, Int::class.java, Int::class.java, Int::class.java,
                String::class.java, String::class.java, String::class.java, Array<UsbConfiguration>::class.java,
                String::class.java, Boolean::class.java, Boolean::class.java, Boolean::class.java, Boolean::class.java,
                Boolean::class.java,
            ).newInstance(name, vid, pid, 0, 0, 0, "TP-Link", "802.11ac NIC", "2.10", arrayOf<UsbConfiguration>(),
                "123456", false, false, false, false, false)
            val reader = Class.forName("android.hardware.usb.IUsbSerialReader")
            val proxy = java.lang.reflect.Proxy.newProxyInstance(reader.classLoader, arrayOf(reader)) { _, m, _ ->
                if (m.returnType == String::class.java) "123456" else null
            }
            return builder.getMethod("build", reader).invoke(b, proxy) as UsbDevice
        }
    }
}
