
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");

importScripts("/fetch/content-encoding/br/big-br-body.https.any.js");
done();
