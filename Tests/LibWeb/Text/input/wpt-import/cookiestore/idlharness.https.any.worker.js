
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
importScripts("/resources/WebIDLParser.js")
importScripts("/resources/idlharness.js")
importScripts("/service-workers/service-worker/resources/test-helpers.sub.js")
importScripts("/cookiestore/idlharness.https.any.js");
done();
