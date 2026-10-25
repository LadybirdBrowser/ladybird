
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "Request disturbed";
importScripts("../resources/utils.js")
importScripts("/fetch/api/request/request-disturbed.any.js");
done();
