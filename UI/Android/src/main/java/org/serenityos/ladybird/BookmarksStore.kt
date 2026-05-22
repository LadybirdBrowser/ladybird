package org.serenityos.ladybird

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

data class Bookmark(val url: String, val title: String, val addedAt: Long)

class BookmarksStore(context: Context) {
    private val file: File = File(context.filesDir, "bookmarks.json")
    private val items: MutableList<Bookmark> = mutableListOf()

    init {
        load()
    }

    fun all(): List<Bookmark> = items.toList()

    fun contains(url: String): Boolean = items.any { it.url == url }

    fun add(url: String, title: String): Boolean {
        if (contains(url)) return false
        items.add(0, Bookmark(url, title, System.currentTimeMillis()))
        save()
        return true
    }

    fun remove(url: String) {
        if (items.removeAll { it.url == url }) save()
    }

    fun clear() {
        items.clear()
        save()
    }

    private fun load() {
        items.clear()
        if (!file.exists()) return
        try {
            val text = file.readText()
            if (text.isBlank()) return
            val arr = JSONArray(text)
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                items.add(
                    Bookmark(
                        obj.optString("url"),
                        obj.optString("title"),
                        obj.optLong("addedAt", System.currentTimeMillis())
                    )
                )
            }
        } catch (_: Throwable) {
            // Ignore corrupted file – start fresh.
        }
    }

    private fun save() {
        val arr = JSONArray()
        for (b in items) {
            val obj = JSONObject()
            obj.put("url", b.url)
            obj.put("title", b.title)
            obj.put("addedAt", b.addedAt)
            arr.put(obj)
        }
        try {
            file.writeText(arr.toString())
        } catch (_: Throwable) {
        }
    }
}
