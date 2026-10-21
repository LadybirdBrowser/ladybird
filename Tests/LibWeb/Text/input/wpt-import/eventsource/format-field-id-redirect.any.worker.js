
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "EventSource: Last-Event-ID after redirect";
importScripts("/eventsource/format-field-id-redirect.any.js");
done();
