
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");

importScripts("/user-timing/structured-serialize-detail.any.js");
done();
