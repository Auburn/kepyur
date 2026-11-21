package com.kepyur.app

import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Color
import android.graphics.Rect
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.InsetDrawable
import android.text.TextUtils
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

enum class Tone { IDLE, BUSY, LIVE, WARN, ERROR }

/** A signal a scan found. */
data class Found(val mhz: Double, val name: String, val snrDb: Float, val std: String)

/** What the menu shows about the adapter and the radio; everything else on it is a [Settings] value. */
data class MenuState(
    val tone: Tone = Tone.IDLE,
    /** Over the preview's top left, beside the dot; not while the picture is [locked]. */
    val headline: String = "",
    /** Along the top of the menu, its whole width; with this and [action] both empty, the app's name instead. */
    val detail: String = "",
    /** What tapping the status does ("Try again"); null: nothing. */
    val action: String? = null,
    /** The picture is locked: the preview shows it, and the headline gives way to it (the dot stays). */
    val locked: Boolean = false,
    /** The radio is up: picking a channel and scanning do something now. */
    val radioReady: Boolean = false,
    val scanning: Boolean = false,
    val scanStep: Int = 0,
    val scanSteps: Int = 0,
    val found: List<Found> = emptyList(),
    /** The last scan has ended and saw nothing. */
    val scanFoundNothing: Boolean = false,
    /** A line of numbers for the Signal tab (USB speed, sample rate, CPU, drops). */
    val stats: String = "",
)

/**
 * The menu: everything the app has to offer, over a black screen. A rail on the left (the four tabs, and at its
 * foot the picture, small, with the status dot and headline over its top left: tapping it closes the menu), the
 * chosen tab's settings on the right, and a line of the status's words along the top, the whole width. It changes
 * [Settings] itself and tells the [Listener] which kind of thing changed; it knows nothing of the radio. The
 * preview is a hole: it tells the [Listener] where it is, and the native screen draws the picture there.
 */
@SuppressLint("ViewConstructor")
class MenuView(ctx: Context, private val s: Settings, private val on: Listener) : FrameLayout(ctx) {
    interface Listener {
        /** A channel was picked ([Settings.channelMhz] and [Settings.band] are already set). */
        fun onTune()
        fun onScan()
        /** A view setting changed (aspect, scaling, size, goggles). */
        fun onView()
        /** A picture switch changed. [rebuild]: one the decoder only takes when the channel is tuned again. */
        fun onPicture(rebuild: Boolean)
        /** A signal setting changed. [retune]: it applies on the next tune. */
        fun onSignal(retune: Boolean)
        /**
         * A view slider is being dragged ([on]) or was let go: while it is, the menu steps aside (all but that
         * slider) so the picture can be seen changing, and the picture should be shown full size.
         */
        fun onPeek(on: Boolean)
        /** A finger has come down somewhere on the menu. */
        fun onTouched() {}
        fun onStatusAction()
        fun onClose()
        /**
         * The preview is now at [left], [top], [right], [bottom] (pixels, in the menu's own coordinates: the screen's,
         * as the menu fills it), its corners rounded by [radius]: the picture goes there while the menu is up.
         */
        fun onPreview(left: Int, top: Int, right: Int, bottom: Int, radius: Float) {}
    }

    private var state = MenuState()
    private var tab = 0
    private val group = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }   // status line, rail + card
    private lateinit var rail: ViewGroup
    private lateinit var railPanel: Panel
    private lateinit var railContent: LinearLayout   // the rail above the preview: the tabs
    private lateinit var preview: PreviewTile
    private lateinit var previewBox: FrameLayout     // the preview and the status badge over it
    private val previewAt = Rect()                   // where the listener was last told the preview is
    private lateinit var card: FrameLayout
    private val pageBodies = ArrayList<LinearLayout>()
    private val pageScrollers = ArrayList<ScrollView>()
    private var peeking: View? = null
    private var peekBody: LinearLayout? = null   // the page whose other rows the peek has faded
    private var peekShade = 255                  // how much of the scrim and the card is showing, 0..255
    private var peekAnim: ValueAnimator? = null
    private var cutLeft = 0    // a camera cutout's share of either end of the screen, px
    private var cutRight = 0

    // status: the dot and the headline over the preview, the detail and the action along the top (the wordmark
    // when there are none)
    private val dot = View(ctx)
    private val wordmark = label(ctx, "KEPYUR", 11f, Skin.TEXT_3, medium = true).apply { letterSpacing = 0.32f }
    internal val statusTitle = label(ctx, "", 13.5f, Skin.TEXT, medium = true).apply {
        maxLines = 1
        ellipsize = TextUtils.TruncateAt.END
        setShadowLayer(dp(3f), 0f, dp(1f), Color.argb(200, 0, 0, 0))   // over the picture, whatever it shows
    }
    internal val statusDetail = label(ctx, "", 12.5f, Skin.TEXT_2).apply {
        maxLines = 1   // a long message in full: the Signal tab
        ellipsize = TextUtils.TruncateAt.END
    }
    internal val statusAction = label(ctx, "", 12.5f, Skin.ACCENT, medium = true)
    internal val statusRow = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.TOP
        setOnClickListener { if (state.action != null) on.onStatusAction() }
    }

    // rail
    private val tabNames = listOf("Channel", "View", "Picture", "Signal")
    private val tabViews = ArrayList<TextView>()
    private val pages = ArrayList<View>()

    // channel page
    private lateinit var bandPicker: Segmented
    private val bandName = label(ctx, "", 12.5f, Skin.TEXT_2)
    private val tiles = List(8) { ChannelTile(ctx) }
    private val tileRows = ArrayList<LinearLayout>()
    private lateinit var scanButton: PillButton
    private val scanText = label(ctx, "", 12.5f, Skin.TEXT_2)
    private val foundRow = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
    private var foundShown: List<Found>? = null

    // view page
    private lateinit var fitRow: SettingRow
    private lateinit var sizeRow: SettingRow
    private lateinit var eyeRow: SettingRow
    private lateinit var eyeDivider: View

    // picture page
    private lateinit var cleanOsdRow: SettingRow

    // signal page
    private lateinit var offsetRow: SettingRow
    private lateinit var gainRow: SettingRow
    private lateinit var gainPicker: Segmented
    private var gainApplied = s.gainDb   // the manual gain the radio was last told
    private lateinit var gainDivider: View
    private val stats = label(ctx, "", 12.5f, Skin.TEXT_2)

    // Everything the pages read while they are built has to be declared above this point: Kotlin runs property
    // initialisers and init blocks in the order they are written.
    private val fitWords = listOf(
        "The whole picture, black bars where the screen is wider",
        "Pulled to the shape of the screen",
        "Fills the screen, the picture's edges cut off",
    )

    init {
        // no background: behind the menu the screen is black but for the picture in the preview
        isClickable = true   // touches stop here: nothing behind the menu reacts
        rail = buildRail()
        railPanel = Panel(Skin.PANEL, dp(22f), Skin.STROKE, dp(1f))
        rail.background = railPanel
        rail.setPadding(dpi(8f), dpi(8f), dpi(8f), dpi(8f))
        card = FrameLayout(ctx).apply {
            background = rounded(Skin.PANEL, dp(22f), Skin.STROKE, dpi(1f))
            clipToOutline = true
        }
        pages += scrolling(buildChannelPage())
        pages += scrolling(buildViewPage())
        pages += scrolling(buildPicturePage())
        pages += scrolling(buildSignalPage())
        for (p in pages) card.addView(p, LayoutParams(-1, -1))
        // along the top, the whole width: a line of the status's words, in from either end as far as the tabs' and
        // the card's words are; under it the rail and the card
        statusRow.setPadding(dpi(22f), 0, dpi(22f), 0)
        statusRow.addView(wordmark, LinearLayout.LayoutParams(-2, -2))
        statusRow.addView(statusDetail, LinearLayout.LayoutParams(0, -2, 1f))
        statusRow.addView(statusAction, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dpi(14f) })
        group.addView(statusRow, LinearLayout.LayoutParams(-1, dpi(STATUS_ROW_DP)))
        val body = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        body.addView(rail, LinearLayout.LayoutParams(dpi(RAIL_DP), -1))
        body.addView(card, LinearLayout.LayoutParams(0, -1, 1f).apply { marginStart = dpi(12f) })
        group.addView(body, LinearLayout.LayoutParams(-1, 0, 1f))
        addView(group, LayoutParams(-1, -1, Gravity.CENTER))
        setOnApplyWindowInsetsListener { _, insets ->
            // keep clear of a camera cutout at either end (the picture runs under it, the menu does not)
            val cut = insets.displayCutout
            val left = cut?.safeInsetLeft ?: 0
            val right = cut?.safeInsetRight ?: 0
            if (left != cutLeft || right != cutRight) {
                cutLeft = left
                cutRight = right
                requestLayout()
            }
            insets
        }
        showTab(0, animate = false)
        syncChannel()
        render(state)
    }

    override fun dispatchTouchEvent(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_DOWN) on.onTouched()
        // While a slider is held and the rest has faded away, what cannot be seen must not be pressed: a second
        // finger (a thumb resting where the preview was) goes nowhere.
        if (peeking != null && e.actionMasked == MotionEvent.ACTION_POINTER_DOWN) return true
        return super.dispatchTouchEvent(e)
    }

    /**
     * On a phone the menu takes the whole screen; on anything larger it stays a phone-sized group in the middle.
     * The channel tiles take what height the card can spare, within reason.
     */
    override fun onMeasure(w: Int, h: Int) {
        val lp = group.layoutParams
        lp.width = min(MeasureSpec.getSize(w), dpi(1010f))
        lp.height = min(MeasureSpec.getSize(h), dpi(470f))
        // the margin round the menu (the status's line inside it at the top): less of it on a short screen, where
        // every line counts, and above and below a third less again than at the sides
        val base = dpi(if (lp.height < dpi(SHORT_DP)) 12f else 18f)
        val ends = (base * VERTICAL_MARGIN).roundToInt()
        group.setPadding(max(base, cutLeft + dpi(8f)), ends, max(base, cutRight + dpi(8f)), ends)
        val cardHeight = lp.height - group.paddingTop - group.paddingBottom - dpi(STATUS_ROW_DP)
        val railHeight = cardHeight - rail.paddingTop - rail.paddingBottom
        fitRail(railHeight)
        fitPreview(railHeight)
        val tileHeight = ((cardHeight - dpi(CHANNEL_PAGE_REST)) / 2).coerceIn(dpi(56f), dpi(84f))
        for (r in tileRows) r.layoutParams.height = tileHeight
        super.onMeasure(w, h)
    }

    /**
     * The rail's panel leaves the preview open, and the listener hears where it is (the picture is drawn there).
     * Whatever moved it (another aspect ratio, another screen), this runs after it.
     */
    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        super.onLayout(changed, l, t, r, b)
        val inRail = Rect(0, 0, preview.width, preview.height)
        rail.offsetDescendantRectToMyCoords(preview, inRail)
        railPanel.hole.set(inRail)
        railPanel.holeRadius = preview.radius
        railPanel.invalidateSelf()
        val at = Rect(0, 0, preview.width, preview.height)
        offsetDescendantRectToMyCoords(preview, at)
        if (at != previewAt) {
            previewAt.set(at)
            on.onPreview(at.left, at.top, at.right, at.bottom, preview.radius)
        }
    }

    // ------------------------------------------------------------------------------------------ rail
    private fun buildRail(): ViewGroup {
        // The tabs at the top, the preview always at the bottom, in place of a Close button; should the tabs not
        // fit above it on some very short screen, they scroll.
        val whole = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        val rail = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        railContent = rail
        for ((i, name) in tabNames.withIndex()) {
            val t = label(context, name, 15f, Skin.TEXT_2).apply {
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dpi(14f), 0, dpi(14f), 0)
                isClickable = true
                setOnClickListener { showTab(i) }
            }
            tabViews += t
            rail.addView(t, LinearLayout.LayoutParams(-1, dpi(42f)).apply { topMargin = if (i == 0) 0 else dpi(2f) })
        }
        val above = ScrollView(context).apply {
            isVerticalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
            addView(rail, ViewGroup.LayoutParams(-1, -2))
        }
        whole.addView(above, LinearLayout.LayoutParams(-1, 0, 1f))

        // the preview, with the status dot and headline over its top left (a finger there still closes the menu)
        val box = FrameLayout(context)
        previewBox = box
        preview = PreviewTile(context).apply { setOnClickListener { on.onClose() } }
        box.addView(preview, LayoutParams(-1, -1))
        val badge = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        dot.background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(Skin.TEXT_3) }
        badge.addView(dot, LinearLayout.LayoutParams(dpi(8f), dpi(8f)))
        badge.addView(statusTitle, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dpi(7f) })
        box.addView(badge, LayoutParams(-2, -2, Gravity.TOP or Gravity.START).apply {
            topMargin = dpi(10f)
            marginEnd = dpi(11f)
            marginStart = dpi(11f)
        })
        whole.addView(box, LinearLayout.LayoutParams(-1, dpi(PREVIEW_MIN_DP)).apply { topMargin = dpi(PREVIEW_GAP_DP) })
        return whole
    }

    /** The rail on a screen [height] px tall inside its padding: lower tabs when it is short. */
    private fun fitRail(height: Int) {
        val tab = dpi(if (height < dpi(SHORT_DP - 40f)) 36f else 42f)
        for (t in tabViews) if (t.layoutParams.height != tab) t.layoutParams.height = tab
    }

    /**
     * The preview takes whatever height of the rail ([height] px inside its padding) the tabs leave, up to the
     * picture's own shape at the rail's width, and no less than [PREVIEW_MIN_DP].
     */
    private fun fitPreview(height: Int) {
        val width = dpi(RAIL_DP) - rail.paddingLeft - rail.paddingRight
        railContent.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED))
        val least = dpi(PREVIEW_MIN_DP)
        val most = max(least, (width / s.aspect).roundToInt())
        val tile = (height - railContent.measuredHeight - dpi(PREVIEW_GAP_DP)).coerceIn(least, most)
        if (previewBox.layoutParams.height != tile) previewBox.layoutParams.height = tile
    }

    fun showTab(index: Int, animate: Boolean = true) {
        endPeek()
        tab = index
        for ((i, t) in tabViews.withIndex()) {
            val sel = i == index
            t.isSelected = sel
            t.background = if (sel) rounded(Skin.FILL_STRONG, dp(13f)) else null
            t.setTextColor(if (sel) Skin.TEXT else Skin.TEXT_2)
            t.typeface = if (sel) Skin.MEDIUM else Skin.REGULAR
        }
        for ((i, p) in pages.withIndex()) {
            p.animate().cancel()
            if (i == index) {
                p.visibility = View.VISIBLE
                if (animate) {
                    p.alpha = 0f
                    p.translationY = dp(6f)
                    p.animate().alpha(1f).translationY(0f).setDuration(160).start()
                } else {
                    p.alpha = 1f
                    p.translationY = 0f
                }
            } else {
                p.visibility = View.GONE
            }
        }
    }

    private fun scrolling(content: View) = ScrollView(context).apply {
        isVerticalScrollBarEnabled = false
        overScrollMode = View.OVER_SCROLL_NEVER
        isFillViewport = true
        // a page taller than the card fades out at the edge it continues past: the only sign that it scrolls
        isVerticalFadingEdgeEnabled = true
        setFadingEdgeLength(dpi(28f))
        addView(content, ViewGroup.LayoutParams(-1, -2))
        pageScrollers += this
    }

    /** A row that has just appeared may be below the card's edge: bring it into view. */
    private fun reveal(row: View) {
        val scroller = pageScrollers.firstOrNull { row.isDescendantOf(it) } ?: return
        scroller.post {
            val bottom = row.bottom + dpi(16f)
            if (bottom > scroller.scrollY + scroller.height) scroller.smoothScrollTo(0, bottom - scroller.height)
        }
    }

    private fun View.isDescendantOf(group: ViewGroup): Boolean {
        var p = parent
        while (p != null) {
            if (p === group) return true
            p = p.parent
        }
        return false
    }

    private fun page() = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dpi(22f), dpi(16f), dpi(22f), dpi(16f))
        // a row being dragged gets a backing a little wider than itself, reaching into the padding (peek)
        clipChildren = false
        clipToPadding = false
        pageBodies += this
    }

    /**
     * While a view slider is dragged ([row], else null): everything but that row fades away, so the picture
     * can be seen changing under the finger, and comes back when the finger lifts.
     */
    private fun peek(row: View?) {
        if (row === peeking) return
        val was = peeking
        peeking = row
        val to = if (row == null) 1f else 0f
        rail.animate().alpha(to).setDuration(PEEK_MS).start()
        statusRow.animate().alpha(to).setDuration(PEEK_MS).start()
        // the page that is faded is the one the row is on, whatever tab is showing by the time it comes back
        val body = (if (row != null) pageBodies[tab] else peekBody) ?: pageBodies[tab]
        peekBody = if (row != null) body else null
        for (i in 0 until body.childCount) {
            val c = body.getChildAt(i)
            if (c !== (row ?: was)) c.animate().alpha(to).setDuration(PEEK_MS).start()
        }
        // a backing for the row left standing, a little wider than it. (A background with insets hands its
        // insets to the view as padding: the row keeps its own.)
        (row ?: was)?.let { r ->
            val pad = intArrayOf(r.paddingLeft, r.paddingTop, r.paddingRight, r.paddingBottom)
            r.background = if (row == null) null
            else InsetDrawable(rounded(Color.argb(176, 10, 12, 15), dp(18f)), -dpi(14f), 0, -dpi(14f), 0)
            r.setPadding(pad[0], pad[1], pad[2], pad[3])
        }
        peekAnim?.cancel()
        peekAnim = ValueAnimator.ofInt(peekShade, if (row == null) 255 else 0).apply {
            duration = PEEK_MS
            addUpdateListener {
                peekShade = it.animatedValue as Int
                card.background.alpha = peekShade
                railPanel.alpha = peekShade
            }
            start()
        }
        on.onPeek(row != null)
    }

    /** Whatever was being dragged, the menu is whole again (the menu is closing, or another tab is wanted). */
    fun endPeek() = peek(null)

    /** For a picture of the peek (the screenshot test): as if the size slider were held. */
    internal fun peekSizeRow(on: Boolean) = peek(if (on) sizeRow else null)

    // ------------------------------------------------------------------------------------------ channel
    private fun buildChannelPage(): View {
        val p = page()
        val head = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        head.addView(caption(context, "Band"), LinearLayout.LayoutParams(0, -2, 1f))
        head.addView(bandName)
        p.addView(head)
        bandPicker = Segmented(context, Channels.bands.map { it.first.toString() }, bandIndex()) { i ->
            s.band = Channels.bands[i].first
            s.save()
            syncChannel()
        }
        p.addView(bandPicker, LinearLayout.LayoutParams(-1, dpi(38f)).apply { topMargin = dpi(8f) })
        for (r in 0 until 2) {
            val line = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            tileRows += line
            for (c in 0 until 4) {
                val tile = tiles[r * 4 + c]
                tile.setOnClickListener {
                    val ch = tile.channel ?: return@setOnClickListener
                    s.channelMhz = ch.mhz
                    s.band = ch.band
                    s.save()
                    syncChannel()
                    on.onTune()
                }
                line.addView(tile, LinearLayout.LayoutParams(0, -1, 1f).apply { if (c > 0) marginStart = dpi(8f) })
            }
            p.addView(line, LinearLayout.LayoutParams(-1, dpi(60f)).apply { topMargin = if (r == 0) dpi(12f) else dpi(8f) })
        }
        val scan = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        scanButton = PillButton(context, "Scan").apply { setOnClickListener { on.onScan() } }
        scan.addView(scanButton, LinearLayout.LayoutParams(-2, dpi(40f)))
        scan.addView(scanText, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dpi(14f) })
        val chips = HorizontalScrollView(context).apply {
            isHorizontalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
            addView(foundRow)
        }
        scan.addView(chips, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dpi(12f) })
        p.addView(scan, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dpi(14f) })
        return p
    }

    private fun bandIndex() = Channels.bands.indexOfFirst { it.first == s.band }.coerceAtLeast(0)

    /** The picker follows [Settings.band] / [Settings.channelMhz] (after a tap here, or when a scan chose a channel). */
    fun syncChannel() {
        bandPicker.select(bandIndex())
        bandName.text = Channels.bands[bandIndex()].second
        val list = Channels.of(s.band)
        for ((i, tile) in tiles.withIndex()) {
            tile.channel = list[i]
            tile.chosen = list[i].mhz == s.channelMhz
            tile.bars = barsOn(list[i])
        }
    }

    private fun barsOn(ch: Channel): Int {
        val f = state.found.filter { abs(it.mhz - ch.mhz) < Channels.SAME_CHANNEL_MHZ }.maxByOrNull { it.snrDb } ?: return 0
        return barsFor(f.snrDb).coerceAtLeast(1)
    }

    // ------------------------------------------------------------------------------------------ view
    private fun buildViewPage(): View {
        val p = page()
        p.addView(SettingRow(context, "Aspect ratio", "How the camera's picture is shaped",
            Segmented(context, listOf("4:3", "16:9"), if (s.wide) 1 else 0) { i ->
                s.wide = i == 1
                requestLayout()   // the preview takes the picture's shape
                changedView()
            }
                .sized(dpi(150f))))
        p.addView(divider(context))
        fitRow = SettingRow(context, "Scaling", fitWords[s.fit],
            Segmented(context, listOf("Fit", "Stretch", "Crop"), s.fit) { i ->
                s.fit = i
                fitRow.detailView.text = fitWords[i]
                changedView()
            }.sized(dpi(216f)))
        p.addView(fitRow)
        p.addView(divider(context))
        sizeRow = SettingRow(context, "Size", sizeWords(),
            Slider(context, Settings.SCALE_MIN, Settings.SCALE_MAX, s.scale, 1f) { v, done ->
                s.scale = v
                sizeRow.detailView.text = sizeWords()
                peek(if (done) null else sizeRow)
                changedView(save = done)
            }.sized(dpi(216f)))
        p.addView(sizeRow)
        p.addView(divider(context))
        p.addView(SettingRow(context, "Goggles", "The picture twice, side by side, for goggles that hold a phone",
            Toggle(context, s.goggles) { v ->
                s.goggles = v
                showEyeRow()
                changedView()
            }))
        eyeDivider = divider(context)
        p.addView(eyeDivider)
        eyeRow = SettingRow(context, "Eye spacing", "Slide until the two pictures become one",
            Slider(context, -1f, 1f, s.eyeSpacing, 0f, fromDetent = true) { v, done ->
                s.eyeSpacing = v
                peek(if (done) null else eyeRow)
                changedView(save = done)
            }.sized(dpi(216f)))
        p.addView(eyeRow)
        showEyeRow()
        return p
    }

    private fun sizeWords() = "${(s.scale * 100).roundToInt()}% of the screen"

    private fun showEyeRow() {
        val was = eyeRow.visibility
        eyeRow.visibility = if (s.goggles) View.VISIBLE else View.GONE
        eyeDivider.visibility = eyeRow.visibility
        if (s.goggles && was != View.VISIBLE) reveal(eyeRow)
    }

    private fun changedView(save: Boolean = true) {
        if (save) s.save()
        on.onView()
    }

    // ------------------------------------------------------------------------------------------ picture
    private fun buildPicturePage(): View {
        val p = page()
        fun toggle(title: String, words: String, value: Boolean, rebuild: Boolean = false, set: (Boolean) -> Unit) =
            SettingRow(context, title, words, Toggle(context, value) { v ->
                set(v)
                s.save()
                on.onPicture(rebuild)
            })
        p.addView(toggle("Comb filter", "A sharper picture with no echo after edges (NTSC cameras)", s.lumaComb) {
            s.lumaComb = it
            cleanOsdRow.setAvailable(it)
        })
        p.addView(divider(context))
        cleanOsdRow = toggle("Clean OSD edges", "No rainbow fringe or halo around text and sharp edges. Needs the comb filter", s.cleanOsd) { s.cleanOsd = it }
        cleanOsdRow.setAvailable(s.lumaComb)
        p.addView(cleanOsdRow)
        p.addView(divider(context))
        p.addView(toggle("Smooth lines", "A steady picture with clean diagonals instead of stair steps", s.smoothLines) { s.smoothLines = it })
        p.addView(divider(context))
        p.addView(toggle("Sparkle repair", "Patches the white and black sparkles of a weak signal", s.sparkleRepair) { s.sparkleRepair = it })
        p.addView(divider(context))
        p.addView(toggle("Colour comb", "Fewer rainbows on fine detail, at the cost of more processing (NTSC cameras)", s.colourComb, rebuild = true) { s.colourComb = it })
        return p
    }

    // ------------------------------------------------------------------------------------------ signal
    private fun buildSignalPage(): View {
        val p = page()
        p.addView(SettingRow(context, "Sample rate", "40 hears more of the channel. 20 is easier on the USB link, and takes over by itself when 40 keeps dropping out",
            Segmented(context, listOf("40 MS/s", "20 MS/s"), if (s.rate40) 0 else 1) { i ->
                s.rate40 = i == 0
                s.save()
                offsetRow.setAvailable(s.rate40)
                on.onSignal(retune = true)
            }.sized(dpi(184f))))
        p.addView(divider(context))
        p.addView(SettingRow(context, "Diversity", "Both of the adapter's antennas combined, so a fade on one is covered by the other",
            Toggle(context, s.diversity) { v -> s.diversity = v; s.save(); on.onSignal(retune = false) }))
        p.addView(divider(context))
        // at 20 MS/s the stream is too narrow to sit beside the channel, and the radio stays centred whatever this says
        offsetRow = SettingRow(context, "Tune beside the channel", "Keeps the picture clear of the chip's blind spot: no stripes at the left edge, no ghost outlines. Needs sample rate 40",
            Toggle(context, s.offsetTune) { v -> s.offsetTune = v; s.save(); on.onSignal(retune = true) })
        offsetRow.setAvailable(s.rate40)
        p.addView(offsetRow)
        p.addView(divider(context))
        gainPicker = Segmented(context, listOf("Auto", "Manual"), if (s.gainAuto) 0 else 1) { i ->
            s.gainAuto = i == 0
            s.save()
            showGainRow()
            on.onSignal(retune = true)
        }.sized(dpi(170f))
        p.addView(SettingRow(context, "Gain", "Automatic suits most flying. By hand for a transmitter so close that it washes the picture out",
            gainPicker))
        gainDivider = divider(context)
        p.addView(gainDivider)
        gainRow = SettingRow(context, "Manual gain", gainWords(),
            Slider(context, 0f, Settings.GAIN_MAX, s.gainDb, 30f) { v, done ->
                s.gainDb = v.roundToInt().toFloat()
                gainRow.detailView.text = gainWords()
                // a retune interrupts the picture: once, when the finger lifts, and only for a new value
                if (done && s.gainDb != gainApplied) {
                    gainApplied = s.gainDb
                    s.save()
                    on.onSignal(retune = true)
                }
            }.apply { spoken = { "${it.roundToInt()} dB" } }.sized(dpi(216f)))
        p.addView(gainRow)
        showGainRow()
        p.addView(divider(context))
        stats.setLineSpacing(dp(3f), 1f)
        p.addView(stats, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dpi(12f) })
        return p
    }

    private fun gainWords() = "${s.gainDb.roundToInt()} dB"

    /** The gain was set from outside the menu (the debug extra): its controls follow [Settings]. */
    fun syncGain() {
        gainApplied = s.gainDb
        (gainRow.control as Slider).set(s.gainDb)
        gainRow.detailView.text = gainWords()
        gainPicker.select(if (s.gainAuto) 0 else 1, animate = false)
        showGainRow()
    }

    private fun showGainRow() {
        val was = gainRow.visibility
        gainRow.visibility = if (s.gainAuto) View.GONE else View.VISIBLE
        gainDivider.visibility = gainRow.visibility
        if (!s.gainAuto && was != View.VISIBLE) reveal(gainRow)
    }

    private fun <T : View> T.sized(width: Int): T = apply { layoutParams = LinearLayout.LayoutParams(width, dpi(38f)) }

    companion object {
        const val RAIL_DP = 204f
        /** A screen lower than this is short: most phones held sideways are, by a little. */
        const val SHORT_DP = 400f
        const val PEEK_MS = 150L
        /** The preview is no lower than this (dp), and sits at least this far below the tabs. */
        const val PREVIEW_MIN_DP = 64f
        const val PREVIEW_GAP_DP = 8f
        /** Along the top, the whole width: one line of the status's words, with a gap under it (dp). */
        const val STATUS_ROW_DP = 24f
        /** The margin above and below the menu, against the one at its sides. */
        const val VERTICAL_MARGIN = 0.65f
        /** Height (dp) of everything on the channel page but its two rows of tiles, paddings included. */
        const val CHANNEL_PAGE_REST = 168f
    }

    // ------------------------------------------------------------------------------------------ state
    fun render(new: MenuState) {
        val st = new.copy(found = new.found.filter { it.mhz.isFinite() && it.mhz > 0 })   // nothing to tune otherwise
        val foundChanged = st.found != state.found
        state = st
        (dot.background as GradientDrawable).setColor(when (st.tone) {
            Tone.LIVE -> Skin.ACCENT
            Tone.BUSY -> Color.WHITE
            Tone.WARN -> Skin.WARN
            Tone.ERROR -> Skin.ERROR
            Tone.IDLE -> Skin.TEXT_3
        })
        // a locked picture speaks for itself: over it only the dot
        if (statusTitle.text.toString() != st.headline) statusTitle.text = st.headline
        statusTitle.visibility = if (st.locked || st.headline.isEmpty()) View.GONE else View.VISIBLE
        // nothing to say: the name of the app in its place (an action alone keeps its place at the end)
        val quiet = st.detail.isEmpty() && st.action == null
        wordmark.visibility = if (quiet) View.VISIBLE else View.GONE
        if (statusDetail.text.toString() != st.detail) statusDetail.text = st.detail
        statusDetail.visibility = if (quiet) View.GONE else if (st.detail.isEmpty()) View.INVISIBLE else View.VISIBLE
        statusAction.text = st.action ?: ""
        statusAction.visibility = if (st.action == null) View.GONE else View.VISIBLE

        scanButton.isEnabled = st.radioReady && !st.scanning
        scanButton.text = if (st.scanning) "Scanning" else "Scan"
        val words = when {
            st.scanning -> "${st.scanStep} of ${st.scanSteps}"
            !st.radioReady -> ""
            st.scanFoundNothing -> "No video found on any channel"
            st.found.isEmpty() -> "Looks for video on every channel"
            else -> ""
        }
        if (scanText.text.toString() != words) scanText.text = words
        scanText.visibility = if (words.isEmpty()) View.GONE else View.VISIBLE
        if (foundChanged || foundShown == null) {
            foundShown = st.found
            foundRow.removeAllViews()
            for (f in st.found.sortedByDescending { it.snrDb }) {
                // a signal further than that from any channel is shown, saved and tuned by its own frequency
                val ch = Channels.nearest(f.mhz, s.band, Channels.SAME_CHANNEL_MHZ)
                val chip = FoundChip(context, ch?.name ?: "${f.mhz.roundToInt()}", barsFor(f.snrDb).coerceAtLeast(1))
                chip.setOnClickListener {
                    s.channelMhz = ch?.mhz ?: f.mhz.roundToInt()
                    if (ch != null) s.band = ch.band
                    s.save()
                    syncChannel()
                    on.onTune()
                }
                foundRow.addView(chip, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dpi(8f) })
            }
            for ((i, tile) in tiles.withIndex()) tile.bars = barsOn(Channels.of(s.band)[i])
        }
        if (stats.text.toString() != st.stats) stats.text = st.stats
    }
}
