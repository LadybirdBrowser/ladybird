
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "Invalid keypath";
importScripts("resources/support.js")
importScripts("/IndexedDB/keypath_invalid.any.js");
done();
