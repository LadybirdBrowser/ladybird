
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "IDBCursor.key";
importScripts("resources/support.js")
importScripts("/IndexedDB/idbcursor-key.any.js");
done();
