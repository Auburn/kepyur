package com.kepyur.app

import android.view.View
import com.kepyur.app.FakeEngine.Call
import com.kepyur.app.UsbLink.State
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.time.Duration
import kotlin.math.abs

/** The one screen, against the fake native side: what it shows, and what it asks of the engine. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = AppHarness.PHONE, shadows = [RecordingUsbManager::class])
class MainActivityScenariosTest : AppHarness() {
    private val f2 = 5760e6

    private fun running() {
        saveChannel(5760, 'F')
        plug(adapter(), broadcast = false)
        launch()
        assertEquals(State.RUNNING, link.state)
        advance(300)
    }

    /** The engine calls that change something, since the last [FakeEngine.clearCalls]. */
    private fun changes() = fake.radioCalls().filter { it.name !in setOf("setSurface", "redraw", "superspeed") }

    private fun assertChanges(vararg expected: Call) = assertEquals(expected.toList(), changes())

    private fun call(name: String, vararg args: Any?) = Call(name, args.toList())

    // ------------------------------------------------------------------------------------------ 1. no adapter
    @Test
    fun launchWithNoAdapterShowsTheMenuAndAppliesTheDefaults() {
        launch()
        advance(300)
        assertEquals(View.VISIBLE, menu.visibility)
        assertEquals("No adapter", headline)
        assertEquals(State.NO_ADAPTER, link.state)
        val c = changes()
        assertTrue(c.toString(), call("setBackdrop", true, true) in c)
        assertTrue(c.toString(), call("setPicture", true, true, true, false) in c)   // luma comb, clean OSD, smooth lines; no repair
        assertTrue(c.toString(), call("setComb", false) in c)
        assertTrue(c.toString(), call("setView", 4f / 3f, Settings.FIT, 1f, false, 0f) in c)
        assertTrue(c.toString(), call("setWatchRate", 1) in c)
        assertTrue(c.toString(), call("setDiversity", false) in c)
        assertTrue(c.toString(), call("setOffsetTuning", true) in c)
        assertTrue(c.toString(), call("setGain", -1.0) in c)   // automatic
        assertEquals(0, fake.count("usb3Switch"))
        assertEquals(0, fake.count("open"))
        assertEquals(0, fake.count("watch"))
        assertEquals(0, fake.count("scan"))
    }

    // ------------------------------------------------------------------------------------------ 2. first run
    @Test
    fun firstRunScansAndAdoptsTheChannelTheScanEndsOn() {
        plug(adapter(), broadcast = false)
        launch()
        assertEquals(State.RUNNING, link.state)
        assertEquals(1, fake.count("open"))
        assertEquals(listOf(call("scan", 0.0)), fake.calls("scan"))
        assertEquals(0, fake.count("watch"))
        advance(300)
        assertEquals("Scanning", headline)

        fake.finishScan(listOf(FakeEngine.Pilot(5769.2, "R4", 31.0)))
        advance(300)
        val s = savedSettings()
        assertEquals(5769, s.channelMhz)
        assertEquals('R', s.band)
        assertTrue(headline, headline.contains("R4") && headline.contains("5769"))
        assertTrue(tile("R4").chosen)
    }

    @Test
    fun firstRunScanThatFindsNothingSaysSo() {
        plug(adapter(), broadcast = false)
        launch()
        fake.finishScan(emptyList())
        advance(300)
        assertEquals("Ready", headline)
        assertTrue(detail, detail.contains("found no video"))
        assertEquals(0, savedSettings().channelMhz)
        advance(10000)   // an idle radio still streams: nothing to recover from
        assertEquals(1, fake.count("open"))
    }

    // ------------------------------------------------------------------------------------------ 3. saved channel
    @Test
    fun savedChannelIsWatchedAtOnceWithoutAScan() {
        running()
        assertEquals(listOf(call("watch", f2, true)), fake.calls("watch"))
        assertEquals(0, fake.count("scan"))
        assertTrue(headline, headline.contains("F2") && headline.contains("5760"))
    }

    // ------------------------------------------------------------------------------------------ 10. menu and hint
    @Test
    fun closeMenuTapForHintHintFadesHintReopensMenuAndBack() {
        launch()
        advance(300)
        fake.clearCalls()
        click(pill("Close menu"))
        assertTrue(changes().toString(), call("setBackdrop", false, false) in changes())
        looper.idleFor(Duration.ofMillis(300))
        assertEquals(View.GONE, menu.visibility)

        tap(root, 300f, 300f)
        assertEquals(View.VISIBLE, hint.visibility)
        looper.idleFor(Duration.ofMillis(2800))
        assertEquals(View.VISIBLE, hint.visibility)
        assertEquals(1f, hint.alpha, 0.01f)
        looper.idleFor(Duration.ofMillis(800))
        assertEquals(View.GONE, hint.visibility)

        tap(root, 300f, 300f)
        assertEquals(View.VISIBLE, hint.visibility)
        fake.clearCalls()
        click(hint)
        assertTrue(changes().toString(), call("setBackdrop", true, false) in changes())
        assertEquals(View.VISIBLE, menu.visibility)
        assertEquals(View.GONE, hint.visibility)
        looper.idleFor(Duration.ofMillis(300))
        assertEquals(1f, menu.alpha, 0.01f)

        // Back from the picture: the menu
        click(pill("Close menu"))
        looper.idleFor(Duration.ofMillis(300))
        @Suppress("DEPRECATION") activity.onBackPressed()
        settle()
        assertEquals(View.VISIBLE, menu.visibility)
        assertFalse(activity.isFinishing)
        // Back from the menu: out of the app
        @Suppress("DEPRECATION") activity.onBackPressed()
        settle()
        assertTrue("Back with the menu open should finish the activity", activity.isFinishing)
    }

    @Test
    fun menuReopenedWhileItIsStillClosingStaysOpen() {
        launch()
        click(pill("Close menu"))
        looper.idleFor(Duration.ofMillis(50))   // the 200 ms fade is under way
        @Suppress("DEPRECATION") activity.onBackPressed()
        looper.idleFor(Duration.ofMillis(600))
        assertEquals(View.VISIBLE, menu.visibility)
        assertEquals(1f, menu.alpha, 0.01f)
    }

    @Test
    fun aTapWhileTheMenuIsOpenShowsNoHint() {
        launch()
        tap(root, 300f, 300f)
        assertEquals(View.GONE, hint.visibility)
    }

    // ------------------------------------------------------------------------------------------ 11. settings
    @Test
    fun pictureSwitchesReachTheEngine() {
        running()
        showTab("Picture")
        val comb = control("Comb filter")
        val clean = control("Clean OSD edges")
        assertTrue(clean.isEnabled)

        fake.clearCalls()
        click(comb)   // off
        assertChanges(call("setPicture", false, false, true, false), call("setComb", false))
        assertFalse(savedSettings().lumaComb)
        assertFalse("Clean OSD edges needs the comb filter", clean.isEnabled)

        fake.clearCalls()
        click(comb)   // on again: clean OSD (still on) counts again
        assertChanges(call("setPicture", true, true, true, false), call("setComb", false))

        fake.clearCalls()
        click(clean)  // off
        assertChanges(call("setPicture", true, false, true, false), call("setComb", false))
        assertFalse(savedSettings().cleanOsd)

        fake.clearCalls()
        click(control("Smooth lines"))
        assertChanges(call("setPicture", true, false, false, false), call("setComb", false))

        fake.clearCalls()
        click(control("Sparkle repair"))
        assertChanges(call("setPicture", true, false, false, true), call("setComb", false))
        assertTrue(savedSettings().sparkleRepair)
    }

    @Test
    fun colourCombIsSetAndTheChannelWatchedAgainNotFresh() {
        running()
        showTab("Picture")
        fake.clearCalls()
        click(control("Colour comb"))
        assertChanges(call("setPicture", true, true, true, false), call("setComb", true), call("watch", f2, false))
        assertTrue(savedSettings().colourComb)
    }

    @Test
    fun signalSettingsReachTheEngine() {
        running()
        showTab("Signal")
        val auto = call("setGain", -1.0)

        fake.clearCalls()
        pick(control("Sample rate") as Segmented, 1, 2)   // 20 MS/s
        assertChanges(call("setWatchRate", 2), auto, call("setDiversity", false), call("setOffsetTuning", true), call("watch", f2, false))
        assertFalse(savedSettings().rate40)

        fake.clearCalls()
        click(control("Diversity"))
        assertChanges(auto, call("setDiversity", true), call("setOffsetTuning", true))   // at once: no re-watch
        assertTrue(savedSettings().diversity)

        fake.clearCalls()
        pick(control("Sample rate") as Segmented, 0, 2)   // back to 40
        assertChanges(call("setWatchRate", 1), auto, call("setDiversity", true), call("setOffsetTuning", true), call("watch", f2, false))

        fake.clearCalls()
        click(control("Tune beside the channel"))
        assertChanges(auto, call("setDiversity", true), call("setOffsetTuning", false), call("watch", f2, false))
        assertFalse(savedSettings().offsetTune)
    }

    @Test
    fun tuningBesideTheChannelIsOnlyOfferedAtSampleRate40() {
        running()
        showTab("Signal")
        val toggle = control("Tune beside the channel")
        val row = toggle.parent as SettingRow
        assertTrue("offered at 40", toggle.isEnabled)

        pick(control("Sample rate") as Segmented, 1, 2)   // 20 MS/s: the radio stays on the channel whatever it says
        assertFalse("not at 20", toggle.isEnabled)
        fake.clearCalls()
        row.performClick()   // the row itself, which works its switch
        toggle.performClick()
        settle()
        assertTrue("a dead control changes nothing: ${fake.calls}", fake.calls.none { it.name == "setOffsetTuning" })
        assertTrue(savedSettings().offsetTune)

        pick(control("Sample rate") as Segmented, 0, 2)
        assertTrue("offered again at 40", toggle.isEnabled)
    }

    @Test
    fun tuningBesideTheChannelIsGreyedOutFromTheStartWhenTheSavedRateIs20() {
        Settings(app).apply { rate40 = false; save() }
        running()
        showTab("Signal")
        val toggle = control("Tune beside the channel")
        val row = toggle.parent as SettingRow
        assertFalse("not offered at 20", toggle.isEnabled)
        assertFalse("nor is the row that works it", row.isEnabled)
        val words = views(row).filterIsInstance<android.widget.TextView>()
        assertTrue("it says why", words.any { it.text.toString().endsWith("Needs sample rate 40") })
        assertTrue("greyed out", words.isNotEmpty() && words.all { it.alpha == 0.4f })

        pick(control("Sample rate") as Segmented, 0, 2)
        assertTrue("offered at 40", toggle.isEnabled)
        assertTrue(row.isEnabled)
    }

    @Test
    fun gainByHandIsAppliedWhenTheSliderIsLetGoAndAutomaticAgainAfter() {
        running()
        showTab("Signal")
        val gainRow = control("Manual gain")
        assertFalse(gainRow.isShown)

        fake.clearCalls()
        pick(control("Gain") as Segmented, 1, 2)   // Manual
        assertChanges(call("setGain", 30.0), call("setDiversity", false), call("setOffsetTuning", true), call("watch", f2, false))
        assertFalse(savedSettings().gainAuto)
        layOut()
        assertTrue(gainRow.isShown)

        fake.clearCalls()
        slide(gainRow as Slider, 0f, Settings.GAIN_MAX, 40f)
        assertChanges(call("setGain", 40.0), call("setDiversity", false), call("setOffsetTuning", true), call("watch", f2, false))
        assertEquals(40f, savedSettings().gainDb, 0.01f)

        fake.clearCalls()
        pick(control("Gain") as Segmented, 0, 2)   // Auto
        assertChanges(call("setGain", -1.0), call("setDiversity", false), call("setOffsetTuning", true), call("watch", f2, false))
        assertTrue(savedSettings().gainAuto)
    }

    @Test
    fun savedManualGainIsAppliedWhenTheRadioOpens() {
        val s = Settings(app)
        s.gainAuto = false
        s.gainDb = 20f
        s.save()
        running()
        val opened = fake.calls.indexOfFirst { it.name == "open" }
        assertTrue(fake.calls.drop(opened).contains(call("setGain", 20.0)))
    }

    private fun assertView(c: Call?, aspect: Float, fit: Int, scale: Float, stereo: Boolean, eyeShift: Float) {
        if (c == null || c.name != "setView") fail("expected setView, got $c")
        val a = c!!.args
        assertEquals("aspect", aspect, a[0] as Float, 0.001f)
        assertEquals("fit", fit, a[1])
        assertEquals("scale", scale, a[2] as Float, 0.02f)
        assertEquals("stereo", stereo, a[3])
        assertEquals("eyeShift", eyeShift, a[4] as Float, 0.003f)
    }

    @Test
    fun viewSettingsReachTheEngine() {
        running()
        showTab("View")

        fake.clearCalls()
        pick(control("Aspect ratio") as Segmented, 1, 2)   // 16:9
        assertEquals(1, changes().size)
        assertView(fake.last("setView"), 16f / 9f, Settings.FIT, 1f, false, 0f)
        assertTrue(savedSettings().wide)

        fake.clearCalls()
        pick(control("Scaling") as Segmented, 2, 3)   // crop
        assertView(fake.last("setView"), 16f / 9f, Settings.CROP, 1f, false, 0f)
        assertEquals(Settings.CROP, savedSettings().fit)

        fake.clearCalls()
        slide(control("Size") as Slider, Settings.SCALE_MIN, Settings.SCALE_MAX, 0.75f)
        assertView(fake.last("setView"), 16f / 9f, Settings.CROP, 0.75f, false, 0f)
        // the picture is shown sharp while the slider is held, the backdrop back when it is let go
        val backdrops = fake.calls("setBackdrop")
        assertEquals(listOf(call("setBackdrop", false, false), call("setBackdrop", true, false)), backdrops)
        assertTrue(abs(savedSettings().scale - 0.75f) < 0.02f)
        assertEquals(0, fake.count("watch"))

        fake.clearCalls()
        click(control("Goggles"))
        assertView(fake.last("setView"), 16f / 9f, Settings.CROP, 0.75f, true, 0f)
        assertTrue(savedSettings().goggles)

        layOut()
        fake.clearCalls()
        slide(control("Eye spacing") as Slider, -1f, 1f, 0.5f)
        assertView(fake.last("setView"), 16f / 9f, Settings.CROP, 0.75f, true, 0.5f * Settings.EYE_SHIFT_MAX)
        assertTrue(abs(savedSettings().eyeSpacing - 0.5f) < 0.03f)
    }

    @Test
    fun channelTileWatchesItAndScanScansFromTheCurrentChannel() {
        running()
        layOut()
        fake.clearCalls()
        click(tile("F4"))
        assertChanges(call("watch", 5800e6, true))
        assertEquals(5800, savedSettings().channelMhz)
        assertEquals('F', savedSettings().band)
        assertTrue(tile("F4").chosen)
        assertFalse(tile("F2").chosen)

        fake.clearCalls()
        click(pill("Scan"))
        assertChanges(call("scan", 5800e6))
        advance(300)
        assertEquals("Scanning", headline)
        assertFalse("Scan is not pressable again while scanning", views(menu).filterIsInstance<PillButton>()
            .first { it.text.toString() == "Scanning" }.isEnabled)
    }

    @Test
    fun channelPickedBeforeTheRadioIsUpIsWatchedOnceItIs() {
        val d = adapter()
        launch()
        layOut()
        click(tile("R1"))
        assertEquals(0, fake.count("watch"))
        assertEquals(5658, savedSettings().channelMhz)
        plug(d)
        assertEquals(State.RUNNING, link.state)
        assertEquals(listOf(call("watch", 5658e6, true)), fake.calls("watch"))
        assertEquals(0, fake.count("scan"))
    }

    // ------------------------------------------------------------------------------------------ 12. lifecycle
    @Test
    fun stopIdlesAndStopsPollingStartWatchesAgainDestroyCloses() {
        running()
        fake.clearCalls()
        controller.pause().stop()
        settle()
        assertEquals(listOf(call("idle")), changes())
        val polls = fake.statusCalls
        advance(2000)
        assertEquals("no status polls while stopped", polls, fake.statusCalls)

        controller.restart().resume()
        settle()
        assertEquals(listOf(call("idle"), call("watch", f2, true)), changes())
        advance(1000)
        assertTrue("polling again", fake.statusCalls > polls)

        controller.pause().stop().destroy()
        settle()
        assertEquals(1, fake.count("close"))
    }

    @Test
    fun aLongStayInTheBackgroundLetsTheAdapterGoAndReturningOpensAndWatchesAgain() {
        running()
        controller.pause().stop()
        settle()
        advance(MainActivity.BACKGROUND_CLOSE_MS - 2000)
        assertEquals("still open just before the limit", 0, fake.count("close"))
        advance(4000)
        assertEquals("closed once the limit has passed", 1, fake.count("close"))

        fake.clearCalls()
        controller.restart().resume()
        waitUntil("the radio up again") { link.state == State.RUNNING }
        settle()
        assertEquals(1, fake.count("open"))
        assertEquals(listOf(call("watch", f2, true)), fake.calls("watch"))
        advance(1000)
        assertEquals(State.RUNNING, link.state)
    }

    @Test
    fun scanCutShortByTheBackgroundStartsOverOnReturn() {
        running()
        click(pill("Scan"))
        settle()
        assertEquals(listOf(call("scan", f2)), fake.calls("scan"))
        controller.pause().stop()
        settle()
        fake.clearCalls()
        controller.restart().resume()
        settle()
        assertEquals("the scan is run again, not silently dropped", listOf(call("scan", f2)), fake.calls("scan"))
    }

    @Test
    fun firstRunRadioThatCameUpInTheBackgroundStillScansOnReturn() {
        plug(adapter(), broadcast = false)
        launch(holdOpen = true)
        controller.pause().stop()
        fake.release()
        waitUntil("the radio up") { link.state == State.RUNNING }
        settle()
        assertEquals(listOf(call("idle")), fake.calls("idle"))
        controller.restart().resume()
        settle()
        assertEquals("first run (no saved channel), back in the foreground: something to show",
            listOf(call("scan", 0.0)), fake.calls("scan"))
    }

    @Test
    fun aFinishedInstanceDestroyedLateDoesNotCloseTheNewInstancesRadio() {
        running()
        val old = controller
        @Suppress("DEPRECATION") activity.onBackPressed()   // menu open: Back leaves the app
        old.pause().stop()
        settle()
        // started again before Android got round to destroying the old instance
        launch()
        assertEquals(State.RUNNING, link.state)
        assertTrue(fake.isOpen)
        old.destroy()
        settle()
        advance(300)
        assertTrue("the new instance's radio was closed by the old one's onDestroy: ${changes()}", fake.isOpen)
    }

    // ------------------------------------------------------------------------------------------ 13. bad status
    /** The scenario as specified: a number the native side printed as nan. */
    @Test
    fun statusWithNanDoesNotCrashAndTheNextGoodOneIsUsed() {
        running()
        fake.rawStatus += """{"open":true,"banks":nan}"""
        advance(500)
        assertEquals(State.RUNNING, link.state)
        assertTrue(fake.rawStatus.isEmpty())
        advance(500)
        assertTrue(headline, headline.contains("F2") && headline.contains("5760"))
        assertEquals(1, fake.count("open"))
        assertEquals(0, fake.count("close"))
    }

    /** Text that org.json cannot read at all (cut short, not JSON): skipped, the next status is used. */
    @Test
    fun unreadableStatusIsSkippedAndTheNextGoodOneIsUsed() {
        running()
        fake.rawStatus += """{"open":true,"banks":"""
        fake.rawStatus += "not json"
        advance(250 * 3)
        assertEquals(State.RUNNING, link.state)
        assertTrue(fake.rawStatus.isEmpty())
        advance(500)
        assertTrue(headline, headline.contains("F2") && headline.contains("5760"))
        assertEquals(0, fake.count("close"))
    }

    /** A complete status with C++'s nan / -inf in the measurements (an idle antenna's dB, a load): shown, no crash. */
    @Test
    fun nanAndInfInTheMeasurementsOfAFullStatusDoNotCrash() {
        running()
        fake.locked = true
        val good = fake.status()
        val bad = good.replace(Regex("\"dsp_load\":[^,]*"), "\"dsp_load\":nan")
            .replace(Regex("\"snr_db\":[^,]*"), "\"snr_db\":-nan")
            .replace(Regex("\"snr_a\":[^,]*"), "\"snr_a\":-inf")
            .replace(Regex("\"fps\":[^,]*"), "\"fps\":inf")
        assertTrue(bad, bad.contains("dsp_load\":nan") && bad.contains("snr_db\":-nan"))
        repeat(3) { fake.rawStatus += bad }
        advance(1000)
        assertEquals(State.RUNNING, link.state)
        assertTrue(headline, headline.contains("F2"))
    }

    /** A pilot whose frequency came out as nan (the scan's measurement, not the app's): no crash. */
    @Test
    fun pilotWithNanFrequencyDoesNotCrashTheMenu() {
        running()
        val good = fake.status()
        val bad = good.replace(Regex("\"pilots\":\\[\\]"), "\"pilots\":[{\"mhz\":nan,\"name\":\"\",\"std\":\"NTSC\",\"snr\":20,\"age_s\":0}]")
        assertTrue(bad, bad.contains("mhz\":nan"))
        repeat(3) { fake.rawStatus += bad }
        advance(1000)
        assertEquals(State.RUNNING, link.state)
    }

    // ------------------------------------------------------------------------------------------ more
    @Test
    fun usbTestRunsBothRatesThenWatchesTheChannel() {
        saveChannel(5760, 'F')
        plug(adapter(), broadcast = false)
        launch(android.content.Intent(app, MainActivity::class.java).putExtra("mode", "usbtest"))
        assertEquals(listOf(call("streamTest", 1, 60.0)), fake.calls("streamTest"))
        assertEquals(0, fake.count("watch"))
        advance(500)
        assertEquals("USB test", headline)
        fake.finishStreamTest(40.0)
        advance(500)
        assertEquals(listOf(call("streamTest", 1, 60.0), call("streamTest", 2, 60.0)), fake.calls("streamTest"))
        advance(2000)
        assertEquals("the second test must not be counted done by the first one's result", 2, fake.count("streamTest"))
        fake.finishStreamTest(20.0)
        advance(500)
        assertEquals(2, fake.count("streamTest"))
        assertEquals(listOf(call("watch", f2, true)), fake.calls("watch"))
    }

    @Test
    fun attachedIntentWhileRunningDoesNothingAndWithNothingOpenOpens() {
        saveChannel(5760, 'F')
        val d = adapter()
        launch()
        plug(d, broadcast = false)   // Android starts the app for the adapter instead of broadcasting first
        controller.newIntent(android.content.Intent(android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED)
            .putExtra(android.hardware.usb.UsbManager.EXTRA_DEVICE, d))
        settle()
        assertEquals(State.RUNNING, link.state)
        assertEquals(1, fake.count("open"))
        controller.newIntent(android.content.Intent(android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED)
            .putExtra(android.hardware.usb.UsbManager.EXTRA_DEVICE, d))
        settle()
        assertEquals(1, fake.count("open"))
        assertEquals(State.RUNNING, link.state)
    }
}
