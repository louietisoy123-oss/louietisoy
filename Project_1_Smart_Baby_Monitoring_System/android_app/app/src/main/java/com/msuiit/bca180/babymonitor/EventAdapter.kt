package com.msuiit.bca180.babymonitor

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.RecyclerView

/**
 * Simple list adapter for the event history screen (GET /api/events).
 * Each row: colored type badge (symbol + type), title line, relative age.
 */
class EventAdapter(
    private var events: List<BabyEvent> = emptyList()
) : RecyclerView.Adapter<EventAdapter.EventViewHolder>() {

    fun submit(newEvents: List<BabyEvent>) {
        events = newEvents
        notifyDataSetChanged()
    }

    class EventViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val tvBadge: TextView = view.findViewById(R.id.tvEventBadge)
        val tvTitle: TextView = view.findViewById(R.id.tvEventTitle)
        val tvAge: TextView = view.findViewById(R.id.tvEventAge)
        val tvConf: TextView = view.findViewById(R.id.tvEventConf)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): EventViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_event, parent, false)
        return EventViewHolder(view)
    }

    override fun getItemCount(): Int = events.size

    override fun onBindViewHolder(holder: EventViewHolder, position: Int) {
        val ctx = holder.itemView.context
        val ev = events[position]

        val (badge, symbol, colorRes, title) = when (ev.type) {
            "cry" -> Quad("CRY", "!", R.color.alert_red,
                if (ev.on) "Cry detected" else "Cry ended")
            "env" -> Quad("ENV", "~", R.color.env_orange,
                if (ev.on) "Environment alert" else "Environment alert ended")
            "fault" -> Quad("FAULT", "!", R.color.fault_amber, "Sensor fault")
            "allclear" -> Quad("OK", "✓", R.color.normal_green, "All clear")
            else -> Quad(ev.type.uppercase(), "?", R.color.text_secondary, "Unknown event")
        }

        holder.tvBadge.text = "$symbol $badge"
        holder.tvBadge.setBackgroundColor(ContextCompat.getColor(ctx, colorRes))
        holder.tvTitle.text = title
        holder.tvTitle.setTextColor(ContextCompat.getColor(ctx, colorRes))
        holder.tvAge.text = MonitorLogic.relativeAgeText(
            ev.recvMs, System.currentTimeMillis(), ev.deviceTsSec)
        holder.tvConf.text = ev.conf?.let { "$it%" } ?: ""
        holder.tvConf.visibility = if (ev.conf != null) View.VISIBLE else View.GONE
    }

    /** Tiny tuple for the `when` mapping above (Kotlin has no inline tuples). */
    private data class Quad(
        val badge: String,
        val symbol: String,
        val colorRes: Int,
        val title: String
    )
}
