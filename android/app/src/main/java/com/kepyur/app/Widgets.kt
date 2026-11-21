package com.kepyur.app

import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorFilter
import android.graphics.Outline
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.animation.DecelerateInterpolator
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.math.abs
import kotlin.math.roundToInt

/** The menu's look: white on dark panels over a black screen, one accent colour. */
object Skin {
    val ACCENT = Color.rgb(61, 220, 180)
    val TEXT = Color.argb(242, 255, 255, 255)
    val TEXT_2 = Color.argb(194, 255, 255, 255)   // what explains
    val TEXT_3 = Color.argb(150, 255, 255, 255)   // what merely labels
    val FILL = Color.argb(22, 255, 255, 255)
    val FILL_STRONG = Color.argb(40, 255, 255, 255)
    val STROKE = Color.argb(30, 255, 255, 255)
    val PANEL = Color.rgb(20, 22, 26)
    val ON_LIGHT = Color.rgb(16, 18, 22)   // text on a white control
    val WARN = Color.rgb(255, 182, 72)
    val ERROR = Color.rgb(255, 112, 112)
    val MEDIUM: Typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    val REGULAR: Typeface = Typeface.create("sans-serif", Typeface.NORMAL)

    fun mix(a: Int, b: Int, t: Float): Int {
        val u = t.coerceIn(0f, 1f)
        fun ch(shift: Int) = ((a ushr shift and 255) + ((b ushr shift and 255) - (a ushr shift and 255)) * u).roundToInt()
        return Color.argb(ch(24), ch(16), ch(8), ch(0))
    }
}

fun View.dp(v: Float): Float = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, resources.displayMetrics)
fun View.dpi(v: Float): Int = dp(v).roundToInt()
fun View.sp(v: Float): Float = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, v, resources.displayMetrics)

fun rounded(color: Int, radius: Float, stroke: Int = 0, strokeWidth: Int = 0) = GradientDrawable().apply {
    setColor(color)
    cornerRadius = radius
    if (strokeWidth > 0) setStroke(strokeWidth, stroke)
}

fun label(ctx: Context, text: String, size: Float, color: Int, medium: Boolean = false) = TextView(ctx).apply {
    this.text = text
    setTextSize(TypedValue.COMPLEX_UNIT_SP, size)
    setTextColor(color)
    typeface = if (medium) Skin.MEDIUM else Skin.REGULAR
    includeFontPadding = false
}

/** A small all-caps heading. */
fun caption(ctx: Context, text: String) = label(ctx, text.uppercase(), 11f, Skin.TEXT_3, medium = true).apply { letterSpacing = 0.14f }

/** A rounded button. [primary]: the one obvious thing to press (white), else quiet glass. */
@SuppressLint("ViewConstructor")
class PillButton(ctx: Context, text: String, private val primary: Boolean = false) : TextView(ctx) {
    init {
        this.text = text
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
        typeface = Skin.MEDIUM
        gravity = Gravity.CENTER
        includeFontPadding = false
        isClickable = true
        isFocusable = true
        minHeight = dpi(44f)
        setPadding(dpi(18f), 0, dpi(18f), 0)
        restyle()
    }

    override fun setEnabled(enabled: Boolean) {
        super.setEnabled(enabled)
        restyle()
    }

    private fun restyle() {
        val r = dp(22f)
        val shape = if (primary) rounded(Color.argb(if (isEnabled) 240 else 90, 255, 255, 255), r)
        else rounded(Skin.FILL, r, Skin.STROKE, dpi(1f))
        val ripple = ColorStateList.valueOf(if (primary) Color.argb(60, 0, 0, 0) else Color.argb(50, 255, 255, 255))
        background = RippleDrawable(ripple, shape, rounded(Color.WHITE, r))
        setTextColor(if (primary) Skin.ON_LIGHT else if (isEnabled) Skin.TEXT else Skin.TEXT_3)
    }
}

/**
 * A panel of the menu: [fill] with rounded corners and a hairline round it, less a [hole] where it has one (the
 * rail, round the preview: the picture under the menu shows through it).
 */
class Panel(private val fill: Int, private val radius: Float, private val stroke: Int, private val strokeWidth: Float) : Drawable() {
    /** The hole, in the same pixels as the bounds; empty: none. */
    val hole = RectF()
    var holeRadius = 0f
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val path = Path()
    private val box = RectF()
    private var shade = 255

    override fun draw(c: Canvas) {
        box.set(bounds)
        path.rewind()
        path.fillType = Path.FillType.EVEN_ODD
        path.addRoundRect(box, radius, radius, Path.Direction.CW)
        if (!hole.isEmpty) path.addRoundRect(hole, holeRadius, holeRadius, Path.Direction.CW)
        paint.style = Paint.Style.FILL
        paint.color = fill
        paint.alpha = Color.alpha(fill) * shade / 255
        c.drawPath(path, paint)
        val half = strokeWidth / 2
        box.inset(half, half)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = strokeWidth
        paint.color = stroke
        paint.alpha = Color.alpha(stroke) * shade / 255
        c.drawRoundRect(box, radius - half, radius - half, paint)
    }

    override fun getOutline(outline: Outline) = outline.setRoundRect(bounds, radius)
    override fun setAlpha(alpha: Int) {
        if (alpha != shade) { shade = alpha; invalidateSelf() }
    }
    override fun getAlpha() = shade
    override fun setColorFilter(filter: ColorFilter?) { paint.colorFilter = filter; invalidateSelf() }
    @Deprecated("Deprecated in Java")
    override fun getOpacity() = PixelFormat.TRANSLUCENT
}

/**
 * The picture, small, at the foot of the menu's rail: a hole the native screen draws the picture into (or NO
 * SIGNAL), with nothing of its own but a hairline and the sign of a full-screen button. Tapping it closes the menu.
 */
class PreviewTile(ctx: Context) : View(ctx) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val box = RectF()
    /** Its corners' radius: the rail's less the rail's padding, so that the two curves run side by side. */
    val radius: Float get() = dp(RADIUS_DP)

    init {
        isClickable = true
        isFocusable = true
        contentDescription = "Close menu"
        foreground = RippleDrawable(ColorStateList.valueOf(Color.argb(60, 255, 255, 255)), null, rounded(Color.WHITE, dp(RADIUS_DP)))
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val half = dp(0.5f)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1f)
        paint.color = Color.argb(46, 255, 255, 255)
        box.set(half, half, w - half, h - half)
        c.drawRoundRect(box, radius - half, radius - half, paint)
        // bottom right, on a dark disc: four corners of a square pointing out
        val r = dp(13f)
        val cx = w - dp(8f) - r
        val cy = h - dp(8f) - r
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(150, 10, 12, 15)
        c.drawCircle(cx, cy, r, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1.6f)
        paint.strokeCap = Paint.Cap.ROUND
        paint.color = Color.WHITE
        val a = dp(4.5f)
        val arm = dp(2.8f)
        for (sx in intArrayOf(-1, 1)) for (sy in intArrayOf(-1, 1)) {
            val x = cx + sx * a
            val y = cy + sy * a
            c.drawLine(x, y, x - sx * arm, y, paint)
            c.drawLine(x, y, x, y - sy * arm, paint)
        }
    }

    companion object {
        const val RADIUS_DP = 14f
    }
}

/** The one thing on screen when the picture is tapped: brings the menu back. */
class MenuHint(ctx: Context) : TextView(ctx) {
    init {
        text = "MENU"
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
        typeface = Skin.MEDIUM
        letterSpacing = 0.22f
        setTextColor(Color.WHITE)
        gravity = Gravity.CENTER
        includeFontPadding = false
        isClickable = true
        background = RippleDrawable(ColorStateList.valueOf(Color.argb(60, 255, 255, 255)),
            rounded(Color.argb(150, 10, 12, 15), dp(24f), Color.argb(70, 255, 255, 255), dpi(1f)), rounded(Color.WHITE, dp(24f)))
    }

    override fun onMeasure(w: Int, h: Int) = setMeasuredDimension(dpi(132f), dpi(48f))
}

/** One choice out of a few, side by side; the white thumb slides to the chosen one. */
@SuppressLint("ViewConstructor")
class Segmented(ctx: Context, private val labels: List<String>, selected: Int, private val onSelect: (Int) -> Unit) : View(ctx) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; typeface = Skin.MEDIUM; textSize = sp(13.5f) }
    private val box = RectF()
    private var thumb = selected.toFloat()   // where the thumb is drawn, in segments
    private var anim: ValueAnimator? = null

    var selected = selected
        private set

    /** Move to [index] without telling the listener. */
    fun select(index: Int, animate: Boolean = true) {
        if (index == selected && anim == null) return
        selected = index
        describe()
        anim?.cancel()
        if (!animate || !isLaidOut) { thumb = index.toFloat(); invalidate(); return }
        anim = ValueAnimator.ofFloat(thumb, index.toFloat()).apply {
            duration = 180
            interpolator = DecelerateInterpolator(1.6f)
            addUpdateListener { thumb = it.animatedValue as Float; invalidate() }
            start()
        }
    }

    /** What this chooses ("Scaling"), for a screen reader: it then says "Scaling: Fit". */
    var label = ""
        set(v) { field = v; describe() }
    private var pressed = -1   // the segment under the finger, while it is down on one

    private fun describe() {
        contentDescription = (if (label.isEmpty()) "" else "$label: ") + labels[selected]
    }

    init { isClickable = true; isFocusable = true; describe() }

    /** Without a pointer (a screen reader's double tap, a switch, a keyboard): on to the next choice, and round. */
    override fun performClick(): Boolean {
        super.performClick()
        if (pressed < 0) {
            select((selected + 1) % labels.size)
            onSelect(selected)
        }
        return true
    }

    override fun onMeasure(w: Int, h: Int) {
        val want = labels.size * dpi(56f)
        setMeasuredDimension(resolveSize(want, w), resolveSize(dpi(38f), h))
    }

    override fun onDraw(c: Canvas) {
        val h = height.toFloat()
        val pad = dp(3f)
        val seg = (width - 2 * pad) / labels.size
        paint.style = Paint.Style.FILL
        paint.color = Skin.FILL
        box.set(0f, 0f, width.toFloat(), h)
        c.drawRoundRect(box, h / 2, h / 2, paint)
        paint.color = Color.argb(238, 255, 255, 255)
        box.set(pad + thumb * seg, pad, pad + (thumb + 1) * seg, h - pad)
        c.drawRoundRect(box, (h - 2 * pad) / 2, (h - 2 * pad) / 2, paint)
        val base = h / 2 - (text.descent() + text.ascent()) / 2
        for (i in labels.indices) {
            text.color = Skin.mix(Skin.TEXT_2, Skin.ON_LIGHT, 1f - abs(thumb - i))
            c.drawText(labels[i], pad + (i + 0.5f) * seg, base, text)
        }
    }

    private fun segmentAt(e: MotionEvent): Int {
        val slop = dp(8f)
        if (e.x < -slop || e.x > width + slop || e.y < -slop || e.y > height + slop) return -1
        return (e.x / width * labels.size).toInt().coerceIn(0, labels.size - 1)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (!isEnabled) return false
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> pressed = segmentAt(e)
            MotionEvent.ACTION_UP -> {
                // the choice is the segment the finger went down on, if it is still there: sliding off cancels
                val i = segmentAt(e)
                if (i >= 0 && i == pressed) {
                    if (i != selected) {
                        select(i)
                        onSelect(i)
                    }
                    performClick()
                }
                pressed = -1
            }
            MotionEvent.ACTION_CANCEL -> pressed = -1
        }
        return true
    }
}

/** An on/off switch. */
@SuppressLint("ViewConstructor")
class Toggle(ctx: Context, on: Boolean, private val onChange: (Boolean) -> Unit) : View(ctx) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val box = RectF()
    private var pos = if (on) 1f else 0f
    private var anim: ValueAnimator? = null

    var isOn = on
        private set

    fun set(on: Boolean, animate: Boolean = true) {
        if (on == isOn) return
        isOn = on
        anim?.cancel()
        val to = if (on) 1f else 0f
        if (!animate || !isLaidOut) { pos = to; invalidate(); return }
        anim = ValueAnimator.ofFloat(pos, to).apply {
            duration = 160
            interpolator = DecelerateInterpolator(1.6f)
            addUpdateListener { pos = it.animatedValue as Float; invalidate() }
            start()
        }
    }

    init { isClickable = true; isFocusable = true }

    override fun setEnabled(enabled: Boolean) {
        super.setEnabled(enabled)
        alpha = if (enabled) 1f else 0.35f
    }

    // a switch, on or off, to anything that asks (a screen reader says so, with the name its row gives it)
    override fun getAccessibilityClassName(): CharSequence = android.widget.Switch::class.java.name

    override fun onInitializeAccessibilityNodeInfo(info: android.view.accessibility.AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        info.isCheckable = true
        info.isChecked = isOn
    }

    override fun onMeasure(w: Int, h: Int) = setMeasuredDimension(dpi(50f), dpi(30f))

    override fun onDraw(c: Canvas) {
        val h = height.toFloat()
        paint.color = Skin.mix(Skin.FILL_STRONG, Skin.ACCENT, pos)
        box.set(0f, 0f, width.toFloat(), h)
        c.drawRoundRect(box, h / 2, h / 2, paint)
        val r = h / 2 - dp(3f)
        paint.color = Color.WHITE
        c.drawCircle(h / 2 + pos * (width - h), h / 2, r, paint)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (!isEnabled) return false
        if (e.actionMasked == MotionEvent.ACTION_UP) {
            val slop = dp(12f)   // lifted on the switch (a little outside counts): sliding away cancels
            if (e.x >= -slop && e.x <= width + slop && e.y >= -slop && e.y <= height + slop) performClick()
        }
        return true
    }

    override fun performClick(): Boolean {
        if (!isEnabled) return false
        super.performClick()
        set(!isOn)
        onChange(isOn)
        return true
    }
}

/**
 * A value between [min] and [max]. Dragging near [detent] snaps to it (and a mark shows where it is).
 * [fromDetent]: a value that goes either way from its detent, drawn filled from there instead of from the left.
 * [onChange] is told every move; the second argument is true when the finger lifts.
 */
@SuppressLint("ViewConstructor")
class Slider(
    ctx: Context, private val min: Float, private val max: Float, value: Float, private val detent: Float,
    private val fromDetent: Boolean = false, private val onChange: (Float, Boolean) -> Unit,
) : View(ctx) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val box = RectF()

    var value = value.coerceIn(min, max)
        private set

    fun set(v: Float) { value = v.coerceIn(min, max); invalidate() }

    override fun onMeasure(w: Int, h: Int) = setMeasuredDimension(resolveSize(dpi(220f), w), resolveSize(dpi(38f), h))

    private val edge get() = dp(12f)
    private fun xOf(v: Float) = edge + (v - min) / (max - min) * (width - 2 * edge)

    override fun onDraw(c: Canvas) {
        val cy = height / 2f
        val t = dp(2.5f)
        paint.color = Skin.FILL_STRONG
        box.set(edge, cy - t, width - edge, cy + t)
        c.drawRoundRect(box, t, t, paint)
        val x = xOf(value)
        val from = if (fromDetent) xOf(detent) else edge
        paint.color = Skin.ACCENT
        box.set(minOf(from, x), cy - t, maxOf(from, x), cy + t)
        c.drawRoundRect(box, t, t, paint)
        paint.color = Skin.TEXT_3
        c.drawCircle(xOf(detent), cy + dp(14f), dp(1.5f), paint)   // clear of the thumb when the value sits on it
        paint.color = Color.WHITE
        c.drawCircle(x, cy, dp(9f), paint)
    }

    private var downX = 0f
    private var downY = 0f
    private var dragging = false

    private fun valueAt(x: Float): Float {
        val v = min + ((x - edge) / (width - 2 * edge)).coerceIn(0f, 1f) * (max - min)
        return if (abs(v - detent) < (max - min) * 0.025f) detent else v
    }

    private fun move(v: Float) {
        if (v == value) return
        value = v
        invalidate()
        onChange(v, false)
    }

    /**
     * As a seek bar does inside something that scrolls: touching it changes nothing. It becomes a drag once the
     * finger has clearly moved along it (until then the page may still take the gesture as a scroll), and a
     * tap, a touch lifted where it landed, sets the value there.
     */
    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (!isEnabled) return false
        val slop = android.view.ViewConfiguration.get(context).scaledTouchSlop
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = e.x
                downY = e.y
                dragging = false
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = abs(e.x - downX)
                if (!dragging && dx > slop && dx > abs(e.y - downY)) {
                    dragging = true
                    parent?.requestDisallowInterceptTouchEvent(true)   // from here on this drag is ours
                }
                if (dragging) move(valueAt(e.x))
            }
            MotionEvent.ACTION_UP -> {
                if (!dragging && abs(e.x - downX) <= slop && abs(e.y - downY) <= slop) move(valueAt(e.x))   // a tap
                finish()
            }
            MotionEvent.ACTION_CANCEL -> finish()
        }
        return true
    }

    private fun finish() {
        parent?.requestDisallowInterceptTouchEvent(false)
        dragging = false
        onChange(value, true)
    }

    // a seek bar to anything that asks: its range, where it stands, and a step either way
    var label = ""
        set(v) { field = v; contentDescription = v }
    /** The value in words ("85%"), for a screen reader. */
    var spoken: (Float) -> String = { "${((it - min) / (max - min) * 100).roundToInt()}%" }

    init { isFocusable = true }

    override fun getAccessibilityClassName(): CharSequence = android.widget.SeekBar::class.java.name

    override fun onInitializeAccessibilityNodeInfo(info: android.view.accessibility.AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        info.rangeInfo = android.view.accessibility.AccessibilityNodeInfo.RangeInfo.obtain(
            android.view.accessibility.AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT, min, max, value)
        if (android.os.Build.VERSION.SDK_INT >= 30) info.stateDescription = spoken(value)
        info.addAction(android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD)
        info.addAction(android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD)
    }

    override fun performAccessibilityAction(action: Int, arguments: android.os.Bundle?): Boolean {
        val step = (max - min) / 20
        val to = when (action) {
            android.view.accessibility.AccessibilityNodeInfo.ACTION_SCROLL_FORWARD -> value + step
            android.view.accessibility.AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD -> value - step
            else -> return super.performAccessibilityAction(action, arguments)
        }
        val v = to.coerceIn(min, max)
        if (v != value) {   // a step, done in one: not a drag in progress followed by its end
            value = v
            invalidate()
            onChange(v, true)
        }
        return true
    }
}

/** Four rising bars, lit by signal strength (0: not drawn at all). */
fun drawBars(c: Canvas, paint: Paint, right: Float, bottom: Float, unit: Float, bars: Int, lit: Int, dim: Int) {
    if (bars <= 0) return
    for (i in 0 until 4) {
        paint.color = if (i < bars) lit else dim
        val x1 = right - (3 - i) * unit * 1.7f
        c.drawRoundRect(x1 - unit, bottom - unit * (1.4f + i), x1, bottom, unit * 0.3f, unit * 0.3f, paint)
    }
}

fun barsFor(snrDb: Float) = when {
    snrDb >= 35 -> 4
    snrDb >= 25 -> 3
    snrDb >= 15 -> 2
    snrDb > 0 -> 1
    else -> 0
}

/** One channel in the picker: its name, its frequency, and signal bars when a scan found video on it. */
class ChannelTile(ctx: Context) : View(ctx) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val name = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Skin.MEDIUM; textSize = sp(19f) }
    private val freq = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Skin.REGULAR; textSize = sp(11.5f) }
    private val box = RectF()
    private var sel = 0f
    private var anim: ValueAnimator? = null

    var channel: Channel? = null
        set(v) { field = v; contentDescription = v?.let { "${it.name}, ${it.mhz} megahertz" }; invalidate() }
    var bars = 0
        set(v) { if (field != v) { field = v; invalidate() } }
    var chosen = false
        set(v) {
            if (field == v) return
            field = v
            isSelected = v
            anim?.cancel()
            val to = if (v) 1f else 0f
            if (!isLaidOut) { sel = to; invalidate(); return }
            anim = ValueAnimator.ofFloat(sel, to).apply {
                duration = 160
                addUpdateListener { sel = it.animatedValue as Float; invalidate() }
                start()
            }
        }

    init { isClickable = true; isFocusable = true }

    override fun onMeasure(w: Int, h: Int) = setMeasuredDimension(resolveSize(dpi(120f), w), resolveSize(dpi(60f), h))

    override fun onDraw(c: Canvas) {
        val ch = channel ?: return
        val r = dp(14f)
        val inset = dp(0.75f)
        box.set(inset, inset, width - inset, height - inset)
        paint.style = Paint.Style.FILL
        paint.color = Skin.mix(if (isPressed) Skin.FILL_STRONG else Skin.FILL, Color.argb(235, 255, 255, 255), sel)
        c.drawRoundRect(box, r, r, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1f)
        paint.color = Skin.mix(Skin.STROKE, Color.TRANSPARENT, sel)
        c.drawRoundRect(box, r, r, paint)
        paint.style = Paint.Style.FILL
        val left = dp(14f)
        name.color = Skin.mix(Skin.TEXT, Skin.ON_LIGHT, sel)
        freq.color = Skin.mix(Skin.TEXT_2, Color.argb(170, 16, 18, 22), sel)
        val mid = height / 2f
        c.drawText(ch.name, left, mid + dp(1f), name)
        c.drawText("${ch.mhz}", left, mid + dp(17f), freq)
        drawBars(c, paint, width - dp(13f), mid + dp(8f), dp(3.2f), bars,
            Skin.mix(Skin.ACCENT, Color.rgb(0, 150, 115), sel), Skin.mix(Skin.FILL_STRONG, Color.argb(50, 0, 0, 0), sel))
    }

    override fun drawableStateChanged() {
        super.drawableStateChanged()
        invalidate()
    }
}

/** A found signal, as a small button: name (or frequency), signal bars. */
@SuppressLint("ViewConstructor")
class FoundChip(ctx: Context, private val title: String, private val bars: Int) : View(ctx) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Skin.MEDIUM; textSize = sp(13.5f); color = Skin.TEXT }
    private val box = RectF()

    init { isClickable = true; contentDescription = title }

    override fun onMeasure(w: Int, h: Int) =
        setMeasuredDimension((text.measureText(title) + dp(52f)).roundToInt(), dpi(36f))

    override fun onDraw(c: Canvas) {
        val h = height.toFloat()
        box.set(dp(0.75f), dp(0.75f), width - dp(0.75f), h - dp(0.75f))
        paint.style = Paint.Style.FILL
        paint.color = if (isPressed) Skin.FILL_STRONG else Skin.FILL
        c.drawRoundRect(box, h / 2, h / 2, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1f)
        paint.color = Skin.STROKE
        c.drawRoundRect(box, h / 2, h / 2, paint)
        paint.style = Paint.Style.FILL
        c.drawText(title, dp(14f), h / 2 - (text.descent() + text.ascent()) / 2, text)
        drawBars(c, paint, width - dp(13f), h / 2 + dp(6f), dp(2.6f), bars, Skin.ACCENT, Skin.FILL_STRONG)
    }

    override fun drawableStateChanged() {
        super.drawableStateChanged()
        invalidate()
    }
}

/** A setting: what it is called and what it does on the left, its control on the right. */
@SuppressLint("ViewConstructor")
class SettingRow(ctx: Context, title: String, detail: String, val control: View) : LinearLayout(ctx) {
    private val titleView = label(ctx, title, 15f, Skin.TEXT, medium = true)
    val detailView = label(ctx, detail, 12.5f, Skin.TEXT_2).apply { setLineSpacing(dp(2f), 1f) }

    init {
        orientation = HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = dpi(58f)
        setPadding(0, dpi(9f), 0, dpi(9f))
        val words = LinearLayout(ctx).apply { orientation = VERTICAL }
        words.addView(titleView)
        words.addView(detailView, LayoutParams(-1, -2).apply { topMargin = dpi(4f) })
        if (detail.isEmpty()) detailView.visibility = GONE
        addView(words, LayoutParams(0, -2, 1f).apply { marginEnd = dpi(20f) })
        addView(control, control.layoutParams ?: LayoutParams(-2, -2))   // a control may come with its own width
        when (control) {
            is Toggle -> {
                // the switch alone is a small thing to hit with a gloved thumb: the whole row works it
                control.contentDescription = title
                setOnClickListener { control.performClick() }
            }
            is Segmented -> control.label = title
            is Slider -> control.label = title
        }
    }

    /** Greyed out, with the control dead (a setting that needs another one on). */
    fun setAvailable(on: Boolean) {
        isEnabled = on   // the row too: it works a switch, and a screen reader must not offer a dead row as an action
        control.isEnabled = on
        titleView.alpha = if (on) 1f else 0.4f
        detailView.alpha = if (on) 1f else 0.4f
    }
}

/** A hairline between settings. */
fun divider(ctx: Context) = View(ctx).apply {
    setBackgroundColor(Color.argb(18, 255, 255, 255))
    layoutParams = LinearLayout.LayoutParams(-1, 1)
}
