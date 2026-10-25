
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "XMLHttpRequest: byte order marks split across chunks";
importScripts("/xhr/split-byte-order-mark.any.js");
done();
