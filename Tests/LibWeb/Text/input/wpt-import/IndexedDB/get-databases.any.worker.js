
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
importScripts("resources/support-promises.js")
importScripts("/IndexedDB/get-databases.any.js");
done();
