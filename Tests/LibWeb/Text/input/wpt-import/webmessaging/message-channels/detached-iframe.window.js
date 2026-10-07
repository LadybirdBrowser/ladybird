// META: title=MessageChannel in a detached iframe test
// META: script=/service-workers/service-worker/resources/test-helpers.sub.js
// META: script=/common/gc.js
// Pull in the with_iframe helper function from the service worker tests


const IframeAction = {
  REMOVE_BEFORE_CREATION: 'remove-before-creation',
  REMOVE_AFTER_CREATION: 'remove-after-creation',
};

async function detached_frame_test(t, action) {
  const iframe = await with_iframe('about:blank');
  const iframe_MessageChannel = iframe.contentWindow.MessageChannel;

  if (action === IframeAction.REMOVE_BEFORE_CREATION) {
    iframe.remove();
  }

  (() => {
    const mc = new iframe_MessageChannel();
    mc.port1.postMessage("boo");
    mc.port2.onmessage = t.unreached_func("message event received");
    mc.port2.onmessageerror = t.unreached_func("message event received");
  })();

  if (action === IframeAction.REMOVE_AFTER_CREATION) {
    iframe.remove();
  }

  await garbageCollect();

  // We are testing that neither of the above two events fire. We assume that a 2 second timeout
  // is good enough. We can't use any other API for an end condition because each MessagePort has
  // its own independent port message queue, which has no ordering guarantees relative to other
  // APIs.
  await new Promise(resolve => t.step_timeout(resolve, 2000));
}

promise_test(async (t) => {
  return detached_frame_test(t, IframeAction.REMOVE_AFTER_CREATION);
}, 'MessageChannel created from a detached iframe should not send messages (remove after create)');

promise_test(async (t) => {
  return detached_frame_test(t, IframeAction.REMOVE_BEFORE_CREATION);
}, 'MessageChannel created from a detached iframe should not send messages (remove before create)');
