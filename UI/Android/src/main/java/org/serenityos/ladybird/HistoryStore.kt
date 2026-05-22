package org.serenityos.ladybird

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

data class HistoryEntry(val url: String, val title: String, val visitedAt: Long)

class HistoryStore(context: Context) {
    private val file: File = File(context.filesDir, "history.json")
    private val entries: MutableList<HistoryEntry> = mutableListOf()
    private val maxEntries = 500

    init {
        load()
    }

    fun all(): List<HistoryEntry> = entries.toList()

    fun record(url: String, title: String) {
        if (url.isBlank() || url == "about:blank") return
        entries.removeAll { it.url == url }
        entries.add(0, HistoryEntry(url, title, System.currentTimeMillis()))
        if (entries.size > maxEntries) {
            entries.subList(maxEntries, entries.size).clear()
        }
        save()
    }

    fun clear() {
        entries.clear()
        save()
    }

    private fun load() {
        entries.clear()
        if (!file.exists()) return
        try {
            val text = file.readText()
            if (text.isBlank()) return
            val arr = JSONArray(text)
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                entries.add(
                    HistoryEntry(
                        obj.optString("url"),
                        obj.optString("title"),
                        obj.optLong("visitedAt", System.currentTimeMillis())
                    )
                )
            }
        } catch (_: Throwable) {
        }
    }

    private fun save() {
        val arr = JSONArray()
        for (h in entries) {
            val obj = JSONObject()
            obj.put("url", h.url)
            obj.put("title", h.title)
            obj.put("visitedAt", h.visitedAt)
            arr.put(obj)
        }
        try {
            file.writeText(arr.toString())
        } catch (_: Throwable) {
        }
    }
}
