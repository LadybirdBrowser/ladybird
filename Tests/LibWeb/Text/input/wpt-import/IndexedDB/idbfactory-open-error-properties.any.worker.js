
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "IndexedDB: Test IDBFactory open() error event properties";
importScripts("resources/support.js")
importScripts("/IndexedDB/idbfactory-open-error-properties.any.js");
done();
