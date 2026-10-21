
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "EventSource: Last-Event-ID (2)";
importScripts("/eventsource/format-field-id-2.any.js");
done();
