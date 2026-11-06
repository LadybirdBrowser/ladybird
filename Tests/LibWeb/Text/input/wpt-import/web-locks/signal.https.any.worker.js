
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "Web Locks API: AbortSignal integration";
importScripts("resources/helpers.js")
importScripts("/web-locks/signal.https.any.js");
done();
