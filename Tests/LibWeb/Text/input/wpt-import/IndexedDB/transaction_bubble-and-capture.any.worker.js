
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "IndexedDB Transaction Event Bubbling and Capturing";
importScripts("resources/support.js")
importScripts("/IndexedDB/transaction_bubble-and-capture.any.js");
done();
