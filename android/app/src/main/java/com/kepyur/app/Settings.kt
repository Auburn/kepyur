package com.kepyur.app

import android.content.Context
import android.content.SharedPreferences
import kotlin.math.abs

/** A standard analog 5.8 GHz channel. The table matches native/dsp/spectrum.cpp (fpv_channels). */
data class Channel(val band: Char, val number: Int, val mhz: Int) {
    val name get() = "$band$number"
}

object Channels {
    /** A signal within this of a channel's frequency is on that channel (VTXs are a few MHz out at worst). */
    const val SAME_CHANNEL_MHZ = 4.0

    /** Band letter to long name, in the order the picker shows them (Raceband first: by far the most common). */
    val bands = listOf('R' to "Raceband", 'F' to "Fatshark", 'A' to "Boscam A", 'B' to "Boscam B", 'E' to "Boscam E", 'L' to "Lowband")

    private val table = mapOf(
        'A' to intArrayOf(5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725),
        'B' to intArrayOf(5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866),
        'E' to intArrayOf(5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945),
        'F' to intArrayOf(5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880),
        'R' to intArrayOf(5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917),
        'L' to intArrayOf(5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621),
    )

    fun of(band: Char): List<Channel> = table.getValue(band).mapIndexed { i, mhz -> Channel(band, i + 1, mhz) }

    /** The channel a frequency belongs to: within [maxOff] MHz, the nearest; [preferBand] wins a tie (F8 and R7 share 5880). */
    fun nearest(mhz: Double, preferBand: Char? = null, maxOff: Double = 6.0): Channel? {
        var best: Channel? = null
        var bestOff = maxOff
        val order = (listOfNotNull(preferBand) + bands.map { it.first }).distinct()
        for (b in order) for (c in of(b)) {
            val off = abs(c.mhz - mhz)
            if (off < bestOff - 0.01) { bestOff = off; best = c }
        }
        return best
    }
}

/** Everything the menu sets, kept across launches. */
class Settings(private val prefs: SharedPreferences) {
    constructor(ctx: Context) : this(ctx.getSharedPreferences("settings", Context.MODE_PRIVATE))

    // channel
    var channelMhz = prefs.getInt("channel_mhz", 0)          // 0: none picked yet
    var band = prefs.getString("band", "R")!!.first()
    // view
    var wide = prefs.getBoolean("wide", false)               // false 4:3, true 16:9
    var fit = prefs.getInt("fit", FIT)
    var scale = prefs.getFloat("scale", 1f)                  // SCALE_MIN..SCALE_MAX
    var goggles = prefs.getBoolean("goggles", false)
    var eyeSpacing = prefs.getFloat("eye_spacing", 0f)       // -1..1: pictures apart .. together
    // picture
    var lumaComb = prefs.getBoolean("luma_comb", true)
    var cleanOsd = prefs.getBoolean("clean_osd", true)
    var smoothLines = prefs.getBoolean("smooth_lines", true)
    var sparkleRepair = prefs.getBoolean("sparkle_repair", false)
    var colourComb = prefs.getBoolean("comb", false)         // the 3-line chroma comb (the key predates this menu)
    // signal
    var rate40 = prefs.getBoolean("rate40", true)
    var diversity = prefs.getBoolean("diversity", false)
    var offsetTune = prefs.getBoolean("offset_tune", true)
    var gainAuto = prefs.getBoolean("gain_auto", true)       // the chip's own gain control
    var gainDb = prefs.getFloat("gain_db", 30f)              // when it is not: 0..GAIN_MAX

    val aspect get() = if (wide) 16f / 9f else 4f / 3f
    /** Fraction of the screen width each eye's picture moves toward the middle. */
    val eyeShift get() = eyeSpacing * EYE_SHIFT_MAX

    fun save() {
        prefs.edit()
            .putInt("channel_mhz", channelMhz).putString("band", band.toString())
            .putBoolean("wide", wide).putInt("fit", fit).putFloat("scale", scale)
            .putBoolean("goggles", goggles).putFloat("eye_spacing", eyeSpacing)
            .putBoolean("luma_comb", lumaComb).putBoolean("clean_osd", cleanOsd)
            .putBoolean("smooth_lines", smoothLines).putBoolean("sparkle_repair", sparkleRepair)
            .putBoolean("comb", colourComb)
            .putBoolean("rate40", rate40).putBoolean("diversity", diversity).putBoolean("offset_tune", offsetTune)
            .putBoolean("gain_auto", gainAuto).putFloat("gain_db", gainDb)
            .apply()
    }

    companion object {
        const val FIT = 0
        const val STRETCH = 1
        const val CROP = 2
        const val SCALE_MIN = 0.5f
        const val SCALE_MAX = 1.5f
        const val EYE_SHIFT_MAX = 0.08f
        const val GAIN_MAX = 45f
    }
}
