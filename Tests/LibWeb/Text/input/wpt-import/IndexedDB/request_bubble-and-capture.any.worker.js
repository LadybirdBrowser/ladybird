
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "Bubbling and capturing of request events";
importScripts("resources/support-promises.js")
importScripts("resources/support.js")
importScripts("/IndexedDB/request_bubble-and-capture.any.js");
done();
