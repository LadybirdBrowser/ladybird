
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("../../../resources/testharness.js");

importScripts("../../../fetch/api/body/textstream.any.js");
done();
