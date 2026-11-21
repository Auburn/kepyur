package com.kepyur.app

import android.hardware.usb.UsbDevice
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.view.accessibility.AccessibilityNodeInfo
import android.widget.TextView
import com.kepyur.app.FakeEngine.Call
import com.kepyur.app.FakeEngine.Pilot
import com.kepyur.app.UsbLink.State
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.time.Duration

/**
 * What the commit "App: what an independent review of the Kotlin side found" changed, against the fake native side:
 * the menu that opens and closes by itself, found signals that are only what the last scan saw, the owed scan,
 * off-grid transmitters, the sliders, switches and choices under a finger and a screen reader, the USB 3 switch
 * that comes back and goes again, and the USB link's own drops.
 *
 * Touches go in through the activity's decor view, at the window position of what is touched, as a finger's do.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = AppHarness.PHONE, shadows = [RecordingUsbManager::class])
class ReviewFixScenariosTest : AppHarness() {
    private val f2 = 5760e6

    /** F2 saved, adapter plugged in with permission, radio up and watching. */
    private fun running(): UsbDevice {
        saveChannel(5760, 'F')
        val d = adapter()
        plug(d, broadcast = false)
        launch()
        assertEquals(State.RUNNING, link.state)
        advance(300)
        return d
    }

    private fun call(name: String, vararg args: Any?) = Call(name, args.toList())

    private val menuShowing get() = menu.visibility == View.VISIBLE

    private fun closeTheMenu() {
        click(pill("Close menu"))
        looper.idleFor(Duration.ofMillis(300))
        assertFalse("the menu should be closed", menuShowing)
    }

    /** Let the menu's fades finish. */
    private fun fades() {
        looper.idleFor(Duration.ofMillis(300))
        settle()
    }

    // ------------------------------------------------------------------------------------------ fingers
    private val decor: View get() = activity.window.decorView

    /** Where ([fx], [fy]) of [v] (fractions of its size) is in the window. */
    private fun at(v: View, fx: Float = 0.5f, fy: Float = 0.5f): FloatArray {
        val loc = IntArray(2)
        v.getLocationInWindow(loc)
        return floatArrayOf(loc[0] + v.width * fx, loc[1] + v.height * fy)
    }

    private fun finger(action: Int, x: Float, y: Float, down: Long) = touch(decor, action, x, y, down)

    /** A tap through the window on the middle of [v]. */
    private fun tapOn(v: View) {
        layOut()
        if (!v.isShown || v.width == 0) fail("$v is not on screen")
        val p = at(v)
        val t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, p[0], p[1], t)
        finger(MotionEvent.ACTION_UP, p[0], p[1], t)
        settle()
    }

    /** The window x of [value] on a laid-out slider whose range is [min]..[max]. */
    private fun xOf(s: Slider, min: Float, max: Float, value: Float): Float {
        val edge = s.dp(12f)
        return at(s, 0f, 0f)[0] + edge + (value - min) / (max - min) * (s.width - 2 * edge)
    }

    private fun midY(v: View) = at(v)[1]

    /** Two fingers on the window: pointer 0 at ([x0], [y0]), pointer 1 at ([x1], [y1]). */
    private fun twoFingers(action: Int, down: Long, x0: Float, y0: Float, x1: Float, y1: Float): Boolean {
        val props = Array(2) { i -> MotionEvent.PointerProperties().apply { id = i; toolType = MotionEvent.TOOL_TYPE_FINGER } }
        val coords = arrayOf(
            MotionEvent.PointerCoords().apply { x = x0; y = y0; pressure = 1f; size = 1f },
            MotionEvent.PointerCoords().apply { x = x1; y = y1; pressure = 1f; size = 1f },
        )
        val e = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, 2, props, coords, 0, 0, 1f, 1f, 0, 0,
            InputDevice.SOURCE_TOUCHSCREEN, 0)
        try {
            return decor.dispatchTouchEvent(e)
        } finally {
            e.recycle()
        }
    }

    private fun pointer1(action: Int) = action or (1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT)

    private fun viewTab() {
        menu.showTab(1, animate = false)
        looper.idle()
        layOut()
    }

    private fun sizeSlider() = control("Size") as Slider

    private fun scaleOf(c: Call?): Float {
        if (c == null || c.name != "setView") fail("expected setView, got $c")
        return c!!.args[2] as Float
    }

    private fun lastBackdrop(): Call? = fake.last("setBackdrop")

    private fun chips(): List<String> = views(menu).filterIsInstance<FoundChip>().map { it.contentDescription.toString() }

    // ------------------------------------------------------------------------------------------ 1. the menu by itself
    @Test
    fun menuClosedUnplugOpensItByItselfAndReplugClosesItAgainAndWatches() {
        val d = running()
        closeTheMenu()
        fake.clearCalls()
        unplug(d)
        settle()
        assertEquals(State.NO_ADAPTER, link.state)
        assertTrue("the menu shows itself when the picture goes", menuShowing)
        assertTrue(fake.calls("setBackdrop").toString(), call("setBackdrop", true, false) in fake.calls("setBackdrop"))
        fades()
        assertEquals("No adapter", headline)

        fake.clearCalls()
        plug(adapter())
        assertEquals(State.RUNNING, link.state)
        fades()
        assertFalse("the menu steps aside again once the radio is up", menuShowing)
        assertEquals(call("setBackdrop", false, false), lastBackdrop())
        assertEquals(listOf(call("watch", f2, true)), fake.calls("watch"))
    }

    @Test
    fun menuThatOpenedItselfAndWasTouchedStaysOpenWhenTheRadioIsBack() {
        val d = running()
        closeTheMenu()
        unplug(d)
        settle()
        assertTrue(menuShowing)
        fades()
        // a finger on the status text (no action in NO_ADAPTER: harmless)
        val status = views(menu).filterIsInstance<TextView>().first { it.text.toString() == "No adapter" }
        tapOn(status)
        assertEquals(State.NO_ADAPTER, link.state)

        fake.clearCalls()
        plug(adapter())
        assertEquals(State.RUNNING, link.state)
        fades()
        assertTrue("touched since it opened: it stays", menuShowing)
        assertFalse(fake.calls("setBackdrop").toString(), call("setBackdrop", false, false) in fake.calls("setBackdrop"))
        assertEquals(listOf(call("watch", f2, true)), fake.calls("watch"))
    }

    @Test
    fun radioDyingWithTheMenuClosedOpensItAndTheReopenClosesIt() {
        running()
        closeTheMenu()
        fake.dead = true
        advanceUntil("the app to close the dead radio", 1000) { fake.count("close") == 1 }
        assertEquals(State.WAITING, link.state)
        assertTrue("the menu shows itself while the radio is started again", menuShowing)
        advanceUntil("the reopen", 4000) { link.state == State.RUNNING }
        fades()
        assertFalse("and closes again once it is back", menuShowing)
        assertEquals(call("setBackdrop", false, false), lastBackdrop())
        assertEquals(List(2) { call("watch", f2, true) }, fake.calls("watch"))
    }

    @Test
    fun menuOpenedByTheUserIsNotClosedByAnUnplugAndReplug() {
        val d = running()
        assertTrue(menuShowing)   // open after launch, as always
        unplug(d)
        settle()
        assertTrue(menuShowing)
        plug(adapter())
        assertEquals(State.RUNNING, link.state)
        fades()
        assertTrue("it was not opened by itself: it stays", menuShowing)
        assertEquals(call("setBackdrop", true, true), lastBackdrop())   // only the launch's
    }

    @Test
    fun adapterGoneDuringAShortAbsenceShowsTheMenuOnReturn() {
        val d = running()
        closeTheMenu()
        controller.pause().stop()
        settle()
        unplug(d)
        settle()
        assertEquals(State.NO_ADAPTER, link.state)
        advance(2000)
        controller.restart().resume()
        settle()
        fades()
        assertTrue("no picture and nothing to say why: the menu should show 'No adapter'", menuShowing)
    }

    // ------------------------------------------------------------------------------------------ 2. long absence
    @Test
    fun longAbsenceShowsTheMenuWhileTheAdapterOpensAndClosesItOnceTheRadioIsUp() {
        running()
        closeTheMenu()
        controller.pause().stop()
        settle()
        advance(MainActivity.BACKGROUND_CLOSE_MS + 1000)
        assertEquals("the adapter was let go", 1, fake.count("close"))

        fake.clearCalls()
        fake.hold()
        controller.restart().resume()
        waitUntil("the worker inside open") { fake.insideOpen == 1 }
        assertEquals(State.OPENING, link.state)
        assertTrue("the menu says why there is no picture yet", menuShowing)
        assertTrue(fake.calls("setBackdrop").toString(), call("setBackdrop", true, false) in fake.calls("setBackdrop"))

        fake.release()
        waitUntil("the radio up again") { link.state == State.RUNNING }
        settle()
        fades()
        assertFalse("closed by itself once the radio is up", menuShowing)
        assertEquals(call("setBackdrop", false, false), lastBackdrop())
        assertEquals(listOf(call("watch", f2, true)), fake.calls("watch"))
    }

    @Test
    fun shortAbsenceDoesNotOpenTheMenu() {
        running()
        closeTheMenu()
        controller.pause().stop()
        settle()
        advance(2000)
        fake.clearCalls()
        controller.restart().resume()
        settle()
        fades()
        assertFalse(menuShowing)
        assertEquals(0, fake.count("setBackdrop"))
        assertEquals(listOf(call("watch", f2, true)), fake.calls("watch"))
    }

    // ------------------------------------------------------------------------------------------ 3. stale scan results
    @Test
    fun secondScanThatSeesNothingNewShowsNoChipsSaysSoAndTunesTheNominalFrequency() {
        plug(adapter(), broadcast = false)
        launch()
        assertEquals(listOf(call("scan", 0.0)), fake.calls("scan"))
        advance(300)
        fake.finishScan(listOf(Pilot(5769.2, "R4", 31.0), Pilot(5760.4, "F2", 12.0)))
        advance(300)
        assertEquals(5769, savedSettings().channelMhz)
        assertEquals(setOf("R4", "F2"), chips().toSet())
        assertTrue("bars on R4", tile("R4").bars > 0)
        // while that scan's result stands, the channel is tuned where its transmitter was measured
        layOut()
        fake.clearCalls()
        click(tile("R4"))
        assertEquals(listOf(call("watch", 5769.2e6, true)), fake.calls("watch"))

        advance(10_000)   // the pilots age: the radio last saw them 10 s ago
        fake.clearCalls()
        click(pill("Scan"))
        assertEquals(1, fake.count("scan"))
        advance(1000)
        fake.finishScan(emptyList())   // nothing new
        advance(300)
        assertEquals("no chips after a scan that saw nothing", emptyList<String>(), chips())
        // the radio is back on the saved channel, looking: the status still says what the scan came to
        assertTrue("status should say the scan found no video: '$headline' / '$detail'", detail.contains("found no video"))
        assertEquals("no bars on R4", 0, tile("R4").bars)

        fake.clearCalls()
        layOut()
        click(tile("R4"))
        assertEquals("the nominal frequency, not the old transmitter's", listOf(call("watch", 5769e6, true)), fake.calls("watch"))
    }

    // ------------------------------------------------------------------------------------------ 4. a scan cut short
    @Test
    fun scanCutShortByALongAbsenceRunsWhenTheRadioIsBackAndNotAgainAfterAShortTrip() {
        running()
        click(pill("Scan"))
        assertEquals(listOf(call("scan", f2)), fake.calls("scan"))
        advance(300)
        controller.pause().stop()
        settle()
        advance(MainActivity.BACKGROUND_CLOSE_MS + 1000)
        assertEquals("the adapter was let go", 1, fake.count("close"))

        fake.clearCalls()
        controller.restart().resume()
        waitUntil("the radio up again") { link.state == State.RUNNING }
        settle()
        assertEquals("the owed scan runs", listOf(call("scan", f2)), fake.calls("scan"))
        advance(300)
        fake.finishScan(emptyList())
        advance(300)

        fake.clearCalls()
        controller.pause().stop()
        settle()
        advance(2000)
        controller.restart().resume()
        settle()
        advance(500)
        assertEquals("no scan is owed any more", 0, fake.count("scan"))
        assertEquals(listOf(call("watch", f2, true)), fake.calls("watch"))
    }

    // ------------------------------------------------------------------------------------------ 5. off-grid transmitter
    @Test
    fun offGridTransmitterIsSavedShownAndTunedByItsOwnFrequency() {
        plug(adapter(), broadcast = false)
        launch()
        advance(300)
        fake.finishScan(listOf(Pilot(5653.0, "R1", 30.0)))   // 5 MHz below R1 (5658): on no channel
        advance(300)
        assertEquals(5653, savedSettings().channelMhz)
        assertFalse("R1 is not chosen", tile("R1").chosen)
        assertTrue("no tile is chosen", views(menu).filterIsInstance<ChannelTile>().none { it.chosen })
        assertEquals(listOf("5653"), chips())

        controller.pause().stop().destroy()
        settle()
        fake.clearCalls()
        launch()
        assertEquals(State.RUNNING, link.state)
        assertEquals(listOf(call("watch", 5653e6, true)), fake.calls("watch"))
        assertEquals(0, fake.count("scan"))
    }

    // ------------------------------------------------------------------------------------------ 6. sliders
    @Test
    fun sliderTapSetsTheValueAtThatPoint() {
        running()
        viewTab()
        val s = sizeSlider()
        val x = xOf(s, Settings.SCALE_MIN, Settings.SCALE_MAX, 0.75f)
        val y = midY(s)
        fake.clearCalls()
        val t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, x, y, t)
        finger(MotionEvent.ACTION_UP, x, y, t)
        settle()
        assertEquals(0.75f, s.value, 0.02f)
        assertEquals(0.75f, scaleOf(fake.last("setView")), 0.02f)
        assertEquals(0.75f, savedSettings().scale, 0.02f)
    }

    @Test
    fun sliderTouchedAndMovedMostlyVerticallyChangesNothing() {
        running()
        viewTab()
        val s = sizeSlider()
        val before = s.value
        val x = xOf(s, Settings.SCALE_MIN, Settings.SCALE_MAX, 0.75f)
        val y = midY(s)
        fake.clearCalls()
        val t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, x, y, t)
        for (i in 1..8) finger(MotionEvent.ACTION_MOVE, x + s.dp(12f) * i / 8, y + s.dp(40f) * i / 8, t)
        finger(MotionEvent.ACTION_UP, x + s.dp(12f), y + s.dp(40f), t)
        settle()
        assertEquals(before, s.value, 0.0001f)
        assertEquals(before, savedSettings().scale, 0.0001f)
        for (c in fake.calls("setView")) assertEquals("setView with another scale: $c", before, scaleOf(c), 0.0001f)
    }

    @Test
    fun sliderDraggedSidewaysFollowsTheFingerAndPeeksUntilItLifts() {
        running()
        viewTab()
        val s = sizeSlider()
        val y = midY(s)
        val x0 = xOf(s, Settings.SCALE_MIN, Settings.SCALE_MAX, 1f)
        val x1 = xOf(s, Settings.SCALE_MIN, Settings.SCALE_MAX, 1.3f)
        fake.clearCalls()
        val t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, x0, y, t)
        val steps = 12
        for (i in 1..steps) {
            finger(MotionEvent.ACTION_MOVE, x0 + (x1 - x0) * i / steps, y, t)
            looper.idle()
        }
        assertEquals("the value follows the finger", 1.3f, s.value, 0.02f)
        assertEquals("while held: the picture sharp", listOf(call("setBackdrop", false, false)), fake.calls("setBackdrop"))
        finger(MotionEvent.ACTION_UP, x1, y, t)
        settle()
        assertEquals(1.3f, s.value, 0.02f)
        assertEquals(1.3f, scaleOf(fake.last("setView")), 0.02f)
        assertEquals(1.3f, savedSettings().scale, 0.02f)
        assertEquals(listOf(call("setBackdrop", false, false), call("setBackdrop", true, false)), fake.calls("setBackdrop"))
    }

    /** Close menu pressed (its click, as the button does) while a slider is held: no blurred picture without a menu. */
    @Test
    fun closeMenuDuringAPeekLeavesThePictureSharpAndTheMenuGone() {
        running()
        viewTab()
        val s = sizeSlider()
        val y = midY(s)
        val x0 = xOf(s, Settings.SCALE_MIN, Settings.SCALE_MAX, 1f)
        val x1 = xOf(s, Settings.SCALE_MIN, Settings.SCALE_MAX, 1.2f)
        val t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, x0, y, t)
        for (i in 1..8) finger(MotionEvent.ACTION_MOVE, x0 + (x1 - x0) * i / 8, y, t)
        assertEquals(call("setBackdrop", false, false), lastBackdrop())
        fake.clearCalls()
        pill("Close menu").performClick()
        settle()
        finger(MotionEvent.ACTION_UP, x1, y, t)   // the first finger lifts
        settle()
        fades()
        assertEquals(fake.calls("setBackdrop").toString(), call("setBackdrop", false, false), lastBackdrop())
        assertFalse("the menu is gone", menuShowing)
    }

    /**
     * The same with a real second finger on Close menu: the commit makes other fingers go nowhere during a peek, so
     * the menu stays; either way the backdrop must follow the menu.
     */
    @Test
    fun secondFingerOnCloseMenuDuringAPeekIsIgnoredAndTheBackdropFollowsTheMenu() {
        running()
        viewTab()
        val s = sizeSlider()
        val y = midY(s)
        val x0 = xOf(s, Settings.SCALE_MIN, Settings.SCALE_MAX, 1f)
        val x1 = xOf(s, Settings.SCALE_MIN, Settings.SCALE_MAX, 1.2f)
        val close = at(pill("Close menu"))
        val t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, x0, y, t)
        for (i in 1..8) finger(MotionEvent.ACTION_MOVE, x0 + (x1 - x0) * i / 8, y, t)
        assertEquals(call("setBackdrop", false, false), lastBackdrop())
        twoFingers(pointer1(MotionEvent.ACTION_POINTER_DOWN), t, x1, y, close[0], close[1])
        twoFingers(MotionEvent.ACTION_MOVE, t, x1, y, close[0], close[1])
        twoFingers(pointer1(MotionEvent.ACTION_POINTER_UP), t, x1, y, close[0], close[1])
        settle()
        finger(MotionEvent.ACTION_UP, x1, y, t)
        settle()
        fades()
        assertTrue("the second finger went nowhere: the menu is still there", menuShowing)
        val last = lastBackdrop()!!
        assertEquals("the backdrop follows the menu: $last, calls ${fake.calls("setBackdrop")}", menuShowing, last.args[0])
        assertEquals(1.2f, s.value, 0.02f)
    }

    /** The control for the test above: the same two-finger events, with no peek, do press Close menu. */
    @Test
    fun secondFingerOnCloseMenuWithNothingPeekingClosesIt() {
        running()
        viewTab()
        val s = sizeSlider()
        val y = midY(s)
        val x0 = xOf(s, Settings.SCALE_MIN, Settings.SCALE_MAX, 1f)
        val close = at(pill("Close menu"))
        val t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, x0, y, t)   // resting on the slider, not dragging it
        twoFingers(pointer1(MotionEvent.ACTION_POINTER_DOWN), t, x0, y, close[0], close[1])
        twoFingers(pointer1(MotionEvent.ACTION_POINTER_UP), t, x0, y, close[0], close[1])
        settle()
        finger(MotionEvent.ACTION_UP, x0, y, t)
        settle()
        fades()
        assertFalse("Close menu pressed by the second finger", menuShowing)
        assertEquals(call("setBackdrop", false, false), lastBackdrop())
    }

    @Test
    fun manualGainTapOnItsOwnValueDoesNotRetuneAndADragRetunesOnceOnLift() {
        running()
        showTab("Signal")
        pick(control("Gain") as Segmented, 1, 2)   // Manual
        assertFalse(savedSettings().gainAuto)
        looper.idleFor(Duration.ofMillis(500))   // the row is scrolled into view
        layOut()
        val g = control("Manual gain") as Slider
        assertTrue(g.isShown)
        assertEquals(30f, g.value, 0.001f)
        val y = midY(g)
        val x30 = xOf(g, 0f, Settings.GAIN_MAX, 30f)

        fake.clearCalls()
        var t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, x30, y, t)
        finger(MotionEvent.ACTION_UP, x30, y, t)
        settle()
        assertEquals("a tap on the value it has: no retune", 0, fake.count("watch"))
        assertEquals(0, fake.count("setGain"))

        val x38 = xOf(g, 0f, Settings.GAIN_MAX, 37.7f)
        fake.clearCalls()
        t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, x30, y, t)
        for (i in 1..10) {
            finger(MotionEvent.ACTION_MOVE, x30 + (x38 - x30) * i / 10, y, t)
            looper.idle()
        }
        assertEquals("nothing retunes while the finger is down", 0, fake.count("watch"))
        finger(MotionEvent.ACTION_UP, x38, y, t)
        settle()
        assertEquals("once, on lift", listOf(call("watch", f2, false)), fake.calls("watch"))
        assertEquals(call("setGain", 38.0), fake.last("setGain"))
        assertEquals(38f, savedSettings().gainDb, 0.001f)
    }

    // ------------------------------------------------------------------------------------------ 7. switches and choices
    @Test
    fun toggleLiftedFarAwayDoesNotFlip() {
        running()
        viewTab()
        val goggles = control("Goggles") as Toggle
        assertFalse(goggles.isOn)
        val p = at(goggles)
        fake.clearCalls()
        val t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, p[0], p[1], t)
        finger(MotionEvent.ACTION_UP, p[0] - goggles.dp(200f), p[1], t)
        settle()
        assertFalse(goggles.isOn)
        assertFalse(savedSettings().goggles)
        assertEquals(0, fake.count("setView"))
        // the same touch lifted where it landed does flip it (so the one above did reach the switch)
        val t2 = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, p[0], p[1], t2)
        finger(MotionEvent.ACTION_UP, p[0], p[1], t2)
        settle()
        assertTrue(goggles.isOn)
        assertEquals(1, fake.count("setView"))
    }

    @Test
    fun tapOnTheRowTitleFlipsTheSwitch() {
        running()
        viewTab()
        val goggles = control("Goggles") as Toggle
        val title = views(menu).filterIsInstance<TextView>().first { it.text.toString() == "Goggles" }
        fake.clearCalls()
        tapOn(title)
        assertTrue(goggles.isOn)
        assertTrue(savedSettings().goggles)
        assertEquals(true, fake.last("setView")?.args?.get(3))
    }

    @Test
    fun segmentedActsOnlyWhenLiftedOnTheSegmentItWentDownOn() {
        running()
        viewTab()
        val seg = control("Scaling") as Segmented
        assertEquals(Settings.FIT, seg.selected)
        val y = midY(seg)
        val left = at(seg, 0f, 0f)[0]
        val fitX = left + seg.width * 0.5f / 3
        val cropX = left + seg.width * 2.5f / 3
        fake.clearCalls()
        var t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, cropX, y, t)
        finger(MotionEvent.ACTION_UP, fitX, y, t)
        settle()
        assertEquals("down on Crop, up on Fit: nothing", Settings.FIT, seg.selected)
        assertEquals(0, fake.count("setView"))

        t = SystemClock.uptimeMillis()
        finger(MotionEvent.ACTION_DOWN, cropX, y, t)
        finger(MotionEvent.ACTION_UP, cropX, y, t)
        settle()
        assertEquals(Settings.CROP, seg.selected)
        assertEquals(1, fake.count("setView"))
        assertEquals(Settings.CROP, fake.last("setView")?.args?.get(1))
        assertEquals(Settings.CROP, savedSettings().fit)
    }

    @Test
    fun segmentedClickWithoutATouchMovesToTheNextChoiceAndRound() {
        running()
        viewTab()
        val seg = control("Scaling") as Segmented
        fake.clearCalls()
        val seen = ArrayList<Int>()
        repeat(3) {
            seg.performClick()
            settle()
            seen += seg.selected
        }
        assertEquals(listOf(Settings.STRETCH, Settings.CROP, Settings.FIT), seen)
        assertEquals(listOf(Settings.STRETCH, Settings.CROP, Settings.FIT), fake.calls("setView").map { it.args[1] })
    }

    // ------------------------------------------------------------------------------------------ 8. USB 3 switch
    @Test
    fun usb3SwitchAdapterBackWithoutPermissionThenGoneAgainEndsAndIsNotSwitchingForEver() {
        saveChannel(5760, 'F')
        fake.switchResults += "requested"
        val d = adapter()
        plug(d, broadcast = false)
        launch()
        assertEquals(State.SWITCHING, link.state)
        unplug(d)   // re-enumeration
        settle()
        assertEquals(State.SWITCHING, link.state)
        val back = adapter()
        plug(back, permission = false)
        assertEquals(State.PERMISSION, link.state)
        advance(2000)
        unplug(back)   // gone again, well within the 14 s
        settle()
        advance(20_000)
        assertTrue("ended, not still switching: ${link.state}", link.state == State.NO_ADAPTER || link.state == State.FAILED)
        assertNotEquals(State.SWITCHING, link.state)
    }

    // ------------------------------------------------------------------------------------------ 9. accessibility
    @Test
    fun controlsSayWhatTheyAreToAScreenReader() {
        running()
        viewTab()
        val goggles = control("Goggles") as Toggle
        var n = goggles.createAccessibilityNodeInfo()
        assertEquals("android.widget.Switch", n.className.toString())
        assertTrue(n.isCheckable)
        assertFalse(n.isChecked)
        assertEquals("Goggles", n.contentDescription?.toString())
        goggles.performClick()
        settle()
        n = goggles.createAccessibilityNodeInfo()
        assertTrue("checked once on", n.isChecked)

        val seg = control("Scaling") as Segmented
        assertEquals("Scaling: Fit", seg.createAccessibilityNodeInfo().contentDescription?.toString())
        pick(seg, 2, 3)
        assertEquals(Settings.CROP, seg.selected)
        assertEquals("Scaling: Crop", seg.createAccessibilityNodeInfo().contentDescription?.toString())

        val s = sizeSlider()
        val r = s.createAccessibilityNodeInfo().rangeInfo
        assertNotNull("the slider has a range", r)
        assertEquals(0.5f, r.min, 0.0001f)
        assertEquals(1.5f, r.max, 0.0001f)
        assertEquals(1f, r.current, 0.0001f)
        fake.clearCalls()
        assertTrue(s.performAccessibilityAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD, null))
        settle()
        assertTrue("raised: ${s.value}", s.value > 1f)
        assertEquals(s.value, scaleOf(fake.last("setView")), 0.0001f)
        assertEquals(s.value, s.createAccessibilityNodeInfo().rangeInfo.current, 0.0001f)
    }

    // ------------------------------------------------------------------------------------------ 10. USB link drops
    private fun statsText(): String =
        (MenuView::class.java.getDeclaredField("stats").apply { isAccessible = true }.get(menu) as TextView).text.toString()

    @Test
    fun usbStreamDropsAreCountedAndSayThePhoneIsNotKeepingUp() {
        running()
        advance(500)
        assertTrue(statsText(), statsText().contains(" 0 dropped"))
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)

        fake.dropped = 2
        fake.streamDropped = 5
        advance(500)
        assertTrue("the numbers count the USB link's drops too: ${statsText()}", statsText().contains(" 7 dropped"))
        assertEquals(MainActivity.OVERLOAD_ADVICE, detail)

        fake.dropped = 0
        fake.streamDropped = 5
        advance(500)
        // stream drops alone, rising
        fake.streamDropped = 9
        advance(500)
        assertTrue(statsText(), statsText().contains(" 9 dropped"))
        assertEquals(MainActivity.OVERLOAD_ADVICE, detail)
        advance(6000)   // no more drops: the warning goes after 5 s
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)
        assertEquals(State.RUNNING, link.state)
    }

    // ------------------------------------------------------------------------------------------ 11. no shared storage
    @Test
    fun theLogGoesToThePrivateFolderWhenSharedStorageIsNotThere() {
        // Android answers null for the shared folder while its volume is not mounted (the first seconds after a
        // boot, for one): the app used to stop on that at launch
        val noStorage = object : android.content.ContextWrapper(app) {
            override fun getExternalFilesDir(type: String?): java.io.File? = null
        }
        assertEquals(app.filesDir, noStorage.dataDir())
        assertEquals(app.getExternalFilesDir(null), app.dataDir())
        assertNotEquals(app.filesDir, app.dataDir())
    }

    @Test
    fun oneLateUsbBlockNowAndThenIsNotThePhoneFailingToKeepUp() {
        running()
        advance(6000)
        // one block the adapter's stream gave up on, then another well apart: any phone stalls for a moment
        fake.streamDropped = 1
        advance(500)
        assertTrue("it is counted: ${statsText()}", statsText().contains(" 1 dropped"))
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)
        advance(8000)
        fake.streamDropped = 2
        advance(500)
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)

        // several close together are another matter
        advance(8000)
        fake.streamDropped = 5
        advance(500)
        assertEquals(MainActivity.OVERLOAD_ADVICE, detail)

        // and one block lost in the queue behind the link always is: that is the phone's own processing
        advance(8000)
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)
        fake.dropped = 1
        advance(500)
        assertEquals(MainActivity.OVERLOAD_ADVICE, detail)
    }

    @Test
    fun threeLateUsbBlocksCountTogetherOnlyWithinTheWindow() {
        running()
        advance(6000)
        // three seen within about four seconds: the link is not being served in time
        fake.streamDropped = 1; advance(2000)
        fake.streamDropped = 2; advance(2000)
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)
        fake.streamDropped = 3; advance(500)
        assertEquals(MainActivity.OVERLOAD_ADVICE, detail)

        // the same three spread over six seconds are not
        advance(10_000)
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)
        fake.streamDropped = 4; advance(3000)
        fake.streamDropped = 5; advance(3000)
        fake.streamDropped = 6; advance(500)
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)

        // and one every three seconds, for a minute, never is
        advance(10_000)
        for (i in 7L..26L) {
            fake.streamDropped = i; advance(3000)
            assertNotEquals("at $i", MainActivity.OVERLOAD_ADVICE, detail)
        }
    }

    @Test
    fun lateUsbBlocksFromTheBackgroundAreNotCountedOnReturn() {
        running()
        advance(6000)
        assertEquals("watch", fake.mode)
        // in the background nothing is refreshed, and the adapter streams on for a while
        controller.pause().stop()
        settle()
        advance(5000); fake.streamDropped = 1
        advance(8000); fake.streamDropped = 2
        advance(8000); fake.streamDropped = 3
        advance(2000)
        controller.restart().resume()
        settle()
        advance(300)
        assertEquals("watch", fake.mode)
        assertNotEquals("three late blocks in 23 s unseen", MainActivity.OVERLOAD_ADVICE, detail)

        // back on screen, the count goes on from where it now stands
        advance(3000)
        fake.streamDropped = 4; advance(500)
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)
        fake.streamDropped = 6; advance(500)
        assertEquals("three seen within the window", MainActivity.OVERLOAD_ADVICE, detail)
    }

    @Test
    fun lateUsbBlocksOfAScanAreNotHeldAgainstTheWatchAfterIt() {
        running()
        advance(6000)
        showTab("Channel")
        click(pill("Scan"))
        settle()
        assertEquals("scan", fake.mode)
        advance(1000)
        fake.streamDropped = 2   // a scan retunes all the time: late blocks there are its own
        advance(1000)
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)
        fake.finishScan(listOf(FakeEngine.Pilot(5760.0, "F2", 31.0)))
        advance(600)
        assertEquals("watch", fake.mode)
        fake.streamDropped = 3   // the first one of the watch
        advance(500)
        assertNotEquals("one late block in this watch", MainActivity.OVERLOAD_ADVICE, detail)
    }

    /** The same for the scan's last moments: blocks that no refresh saw while it was still scanning. */
    @Test
    fun lateUsbBlocksOfAScansLastMomentsAreNotHeldAgainstTheWatchAfterIt() {
        running()
        advance(6000)
        showTab("Channel")
        click(pill("Scan"))
        settle()
        assertEquals("scan", fake.mode)
        advance(1000)
        fake.streamDropped = 2   // after the screen's last look at the scan
        fake.finishScan(listOf(FakeEngine.Pilot(5760.0, "F2", 31.0)))
        advance(600)
        assertEquals("watch", fake.mode)
        assertNotEquals(MainActivity.OVERLOAD_ADVICE, detail)
        fake.streamDropped = 3   // the first one of the watch
        advance(500)
        assertNotEquals("one late block in this watch", MainActivity.OVERLOAD_ADVICE, detail)
        fake.streamDropped = 6   // and the watch's own are still counted
        advance(500)
        assertEquals(MainActivity.OVERLOAD_ADVICE, detail)
    }

    /**
     * And for a stream test: the refresh that sees the last test done starts the watch itself, in the same pass.
     * The radio was not watching at that refresh, so what comes in before the next one is not the watch's.
     */
    @Test
    fun lateUsbBlocksOfTheRetuneAfterAStreamTestAreNotHeldAgainstTheWatch() {
        saveChannel(5760, 'F')
        plug(adapter(), broadcast = false)
        launch(android.content.Intent(app, MainActivity::class.java).putExtra("mode", "usbtest"))
        advance(500)
        fake.finishStreamTest(40.0)
        advance(2500)
        fake.finishStreamTest(20.0)
        toNextPoll()   // reads idle with the test done: the next test, or none left and the watch
        assertEquals("watch", fake.mode)
        fake.streamDropped = 3   // the retune into the watch
        toNextPoll()
        advance(300)
        assertNotEquals("blocks from before the watch's first refresh", MainActivity.OVERLOAD_ADVICE, detail)
        fake.streamDropped = 6   // the watch's own
        advance(500)
        assertEquals(MainActivity.OVERLOAD_ADVICE, detail)
    }

    /** One ms at a time until the status has been read once more (a refresh). */
    private fun toNextPoll() {
        val n = fake.statusCalls
        var steps = 0
        while (fake.statusCalls == n) {
            looper.idleFor(Duration.ofMillis(1))
            settle()
            if (++steps > 2000) fail("no status poll in 2 s")
        }
    }
}
