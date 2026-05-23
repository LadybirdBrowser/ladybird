package org.serenityos.ladybird

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

data class UrlRow(val url: String, val title: String)

class UrlListAdapter(
    private var rows: MutableList<UrlRow>,
    private val onClick: (UrlRow) -> Unit,
    private val onDelete: ((UrlRow) -> Unit)?
) : RecyclerView.Adapter<UrlListAdapter.VH>() {

    class VH(itemView: View) : RecyclerView.ViewHolder(itemView) {
        val title: TextView = itemView.findViewById(R.id.urlTitle)
        val subtitle: TextView = itemView.findViewById(R.id.urlSubtitle)
        val delete: ImageButton = itemView.findViewById(R.id.urlDelete)
        val favicon: ImageView = itemView.findViewById(R.id.urlFavicon)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val v = LayoutInflater.from(parent.context).inflate(R.layout.item_url_row, parent, false)
        return VH(v)
    }

    override fun onBindViewHolder(holder: VH, position: Int) {
        val row = rows[position]
        val title = row.title.ifBlank { row.url }
        holder.title.text = title
        holder.subtitle.text = row.url
        holder.itemView.setOnClickListener { onClick(row) }
        if (onDelete != null) {
            holder.delete.visibility = View.VISIBLE
            holder.delete.setOnClickListener { onDelete.invoke(row) }
        } else {
            holder.delete.visibility = View.GONE
        }
    }

    override fun getItemCount(): Int = rows.size

    fun submit(newRows: List<UrlRow>) {
        rows = newRows.toMutableList()
        notifyDataSetChanged()
    }

    fun removeAt(index: Int) {
        if (index < 0 || index >= rows.size) return
        rows.removeAt(index)
        notifyItemRemoved(index)
    }
}
