
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
importScripts("./resources/ranges.js")
importScripts("/encoding/legacy-mb-schinese/gb18030/gb18030-decoder.any.js");
done();
