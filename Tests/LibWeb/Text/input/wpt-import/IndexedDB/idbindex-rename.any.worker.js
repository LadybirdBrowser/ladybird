
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "IndexedDB: index renaming support";
importScripts("resources/support-promises.js")
importScripts("/IndexedDB/idbindex-rename.any.js");
done();
