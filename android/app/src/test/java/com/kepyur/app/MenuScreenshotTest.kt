package com.kepyur.app

import android.app.Activity
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.os.Looper
import android.view.View
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File

/**
 * Renders [MenuView] off screen to PNGs, for looking at the menu without a device.
 * The bitmaps start black, as the screen behind the menu is, with a stand-in for the picture where the preview
 * is (on the phone the native screen draws the real one there, through the hole the menu leaves).
 *
 *   gradle testDebugUnitTest --tests com.kepyur.app.MenuScreenshotTest -Pshots=/some/dir
 *
 * Without -Pshots the PNGs go to app/build/menu-shots.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [35])
class MenuScreenshotTest {
    private val outDir: File by lazy {
        File(System.getProperty("kepyur.shots") ?: "build/menu-shots").also { it.mkdirs() }
    }

    /** Hears where the preview is, and whether a slider has the picture full size, and nothing else. */
    private class Quiet : MenuView.Listener {
        val preview = RectF()
        var radius = 0f
        var peeking = false
        override fun onTune() {}
        override fun onScan() {}
        override fun onView() {}
        override fun onPicture(rebuild: Boolean) {}
        override fun onSignal(retune: Boolean) {}
        override fun onPeek(on: Boolean) { peeking = on }
        override fun onStatusAction() {}
        override fun onClose() {}
        override fun onPreview(left: Int, top: Int, right: Int, bottom: Int, radius: Float) {
            preview.set(left.toFloat(), top.toFloat(), right.toFloat(), bottom.toFloat())
            this.radius = radius
        }
    }

    /**
     * A sky over a field where the picture is, so that it can be told from the panels: in the preview, or the whole
     * screen while a slider is held (a peek).
     */
    private fun standIn(c: Canvas, at: Quiet) {
        val where = if (at.peeking) RectF(0f, 0f, c.width.toFloat(), c.height.toFloat()) else at.preview
        if (where.isEmpty) return
        val p = Paint(Paint.ANTI_ALIAS_FLAG)
        p.shader = LinearGradient(0f, where.top, 0f, where.bottom, Color.rgb(70, 140, 200), Color.rgb(150, 110, 60),
            Shader.TileMode.CLAMP)
        val r = if (at.peeking) 0f else at.radius
        c.drawRoundRect(where, r, r, p)
    }

    private val live = MenuState(
        tone = Tone.LIVE, headline = "R4 · 5769 MHz", detail = "NTSC · 60 fps · 27 dB", radioReady = true, locked = true,
        found = listOf(Found(5769.2, "R4", 31f, "NTSC"), Found(5843.0, "R6", 22f, "NTSC"), Found(5760.4, "F2", 12f, "PAL")),
        stats = "USB 3 · 40 MS/s · processing 46% · 0 dropped · 0 restarts",
    )

    private fun shoot(name: String, tab: Int, state: MenuState, w: Int, h: Int, setup: Settings.() -> Unit = {},
                      then: (MenuView) -> Unit = {}) {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        val prefs = activity.getSharedPreferences("t-$name", Context.MODE_PRIVATE)
        prefs.edit().clear().commit()
        val settings = Settings(prefs).apply(setup)
        val quiet = Quiet()
        val menu = MenuView(activity, settings, quiet)
        activity.setContentView(menu)
        menu.render(state)
        menu.showTab(tab, animate = false)
        shadowOf(Looper.getMainLooper()).idle()
        layout(menu, w, h)
        shadowOf(Looper.getMainLooper()).idle()
        layout(menu, w, h)
        then(menu)
        shadowOf(Looper.getMainLooper()).idleFor(java.time.Duration.ofMillis(600))   // let a fade finish
        val bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        bitmap.eraseColor(Color.BLACK)
        val canvas = Canvas(bitmap)
        standIn(canvas, quiet)
        menu.draw(canvas)
        File(outDir, "$name.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    private fun layout(v: View, w: Int, h: Int) {
        v.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY))
        v.layout(0, 0, w, h)
    }

    private val tuned: Settings.() -> Unit = { band = 'R'; channelMhz = 5769 }

    // ---------------------------------------------------------------- phone, 2400 x 1080 at 420 dpi
    @Test @Config(qualifiers = PHONE)
    fun channelLive() = shoot("channel_live", 0, live, 2400, 1080, tuned)

    @Test @Config(qualifiers = PHONE)
    fun channelScanning() = shoot("channel_scanning", 0, MenuState(
        tone = Tone.BUSY, headline = "Scanning", detail = "Looking for video on every channel.", radioReady = true,
        scanning = true, scanStep = 7, scanSteps = 29), 2400, 1080, tuned)

    @Test @Config(qualifiers = PHONE)
    fun channelNoAdapter() = shoot("channel_noadapter", 0, MenuState(
        tone = Tone.IDLE, headline = "No adapter", detail = "Plug the Wi-Fi adapter into the phone's USB port."), 2400, 1080)

    @Test @Config(qualifiers = PHONE)
    fun channelError() = shoot("channel_error", 0, MenuState(
        tone = Tone.WARN, headline = "Trying again",
        detail = "The radio did not start: bulk transfer timed out while loading firmware", action = "Try now"), 2400, 1080)

    /** Nothing to say along the top: the name of the app there instead. */
    @Test @Config(qualifiers = PHONE)
    fun channelQuiet() = shoot("channel_quiet", 0, MenuState(tone = Tone.BUSY, headline = "Working", radioReady = true),
        2400, 1080, tuned)

    @Test @Config(qualifiers = PHONE)
    fun view() = shoot("view", 1, live, 2400, 1080, tuned)

    @Test @Config(qualifiers = PHONE)
    fun viewPeek() = shoot("view_peek", 1, live, 2400, 1080, { tuned(); scale = 0.8f }) { it.peekSizeRow(true) }

    /** The one thing on screen when the picture is tapped. */
    @Test @Config(qualifiers = PHONE)
    fun hint() {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        val hint = MenuHint(activity)
        hint.measure(View.MeasureSpec.UNSPECIFIED, View.MeasureSpec.UNSPECIFIED)
        hint.layout(0, 0, hint.measuredWidth, hint.measuredHeight)
        val bitmap = Bitmap.createBitmap(hint.measuredWidth, hint.measuredHeight, Bitmap.Config.ARGB_8888)
        bitmap.eraseColor(Color.TRANSPARENT)
        hint.draw(Canvas(bitmap))
        File(outDir, "hint.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    @Test @Config(qualifiers = PHONE)
    fun viewGoggles() = shoot("view_goggles", 1, live, 2400, 1080, {
        tuned(); goggles = true; wide = true; fit = Settings.CROP; scale = 0.85f; eyeSpacing = 0.3f
    })

    @Test @Config(qualifiers = PHONE)
    fun picture() = shoot("picture", 2, live, 2400, 1080, tuned)

    @Test @Config(qualifiers = PHONE)
    fun pictureNoComb() = shoot("picture_nocomb", 2, live, 2400, 1080, { tuned(); lumaComb = false })

    @Test @Config(qualifiers = PHONE)
    fun signal() = shoot("signal", 3, live, 2400, 1080, tuned)

    @Test @Config(qualifiers = PHONE)
    fun signalDiversity() = shoot("signal_diversity", 3, live.copy(
        stats = "USB 3 · 40 MS/s · processing 58% · 0 dropped · 0 restarts\nAntenna A 24 dB · B 19 dB · together 26 dB"),
        2400, 1080, { tuned(); diversity = true })

    @Test @Config(qualifiers = PHONE)
    fun signalManualGain() = shoot("signal_gain", 3, live, 2400, 1080, { tuned(); gainAuto = false; gainDb = 24f })

    // ---------------------------------------------------------------- small phone, 1600 x 720 at 320 dpi
    @Test @Config(qualifiers = SMALL)
    fun smallChannel() = shoot("small_channel", 0, live, 1600, 720, tuned)

    @Test @Config(qualifiers = SMALL)
    fun smallPicture() = shoot("small_picture", 2, live, 1600, 720, tuned)

    /** A short screen and a long status: the line of the status along the top, the tabs and the preview all fit. */
    @Test @Config(qualifiers = SMALL)
    fun smallNoAdapter() = shoot("small_noadapter", 0, MenuState(
        tone = Tone.IDLE, headline = "No adapter", detail = "Plug the Wi-Fi adapter into the phone's USB port."), 1600, 720)

    @Test @Config(qualifiers = SMALL)
    fun smallError() = shoot("small_error", 1, MenuState(
        tone = Tone.WARN, headline = "Trying again",
        detail = "The radio stopped: no samples for 4 seconds. If this goes on, unplug the adapter and plug it in again.",
        action = "Try now"), 1600, 720)

    /** Narrower still: the same screen with a camera cutout taking 68 dp of its width. */
    @Test @Config(qualifiers = "w732dp-h360dp-land-xhdpi")
    fun narrowView() = shoot("narrow_view", 1, MenuState(
        tone = Tone.IDLE, headline = "No adapter", detail = "Plug the Wi-Fi adapter into the phone's USB port."), 1464, 720,
        { goggles = true })

    // ---------------------------------------------------------------- tablet, 2560 x 1600 at 320 dpi
    @Test @Config(qualifiers = TABLET)
    fun tabletChannel() = shoot("tablet_channel", 0, live, 2560, 1600, tuned)

    companion object {
        const val PHONE = "w914dp-h411dp-land-420dpi"
        const val SMALL = "w800dp-h360dp-land-xhdpi"
        const val TABLET = "w1280dp-h800dp-land-xhdpi"
    }
}
