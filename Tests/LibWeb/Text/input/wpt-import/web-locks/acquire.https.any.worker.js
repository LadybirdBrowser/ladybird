
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "Web Locks API: navigator.locks.request method";
importScripts("resources/helpers.js")
importScripts("/web-locks/acquire.https.any.js");
done();
