
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
importScripts("/resources/WebIDLParser.js")
importScripts("/resources/idlharness.js")
importScripts("/server-timing/idlharness.https.any.js");
done();
