
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
importScripts("../resources/utils.js")
importScripts("/fetch/api/basic/integrity.sub.any.js");
done();
