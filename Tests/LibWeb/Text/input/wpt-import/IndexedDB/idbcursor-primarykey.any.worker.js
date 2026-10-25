
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "IDBCursor.primaryKey";
importScripts("resources/support.js")
importScripts("/IndexedDB/idbcursor-primarykey.any.js");
done();
