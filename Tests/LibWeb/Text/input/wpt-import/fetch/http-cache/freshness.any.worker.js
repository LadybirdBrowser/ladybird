
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "HTTP Cache - Freshness";
importScripts("/common/utils.js")
importScripts("/common/get-host-info.sub.js")
importScripts("http-cache.js")
importScripts("/fetch/http-cache/freshness.any.js");
done();
