// How long (in ms) these tests should wait before deciding no further messages
// will be received.
const time_to_wait_for_messages = 100;

async_test(t => {
    const c = new MessageChannel();
    c.port1.onmessage = t.unreached_func('Should not have delivered message');
    c.port1.close();
    c.port2.postMessage('TEST');
    setTimeout(t.step_func_done(), time_to_wait_for_messages);
  }, 'Message sent to closed port should not arrive.');

async_test(t => {
    const c = new MessageChannel();
    c.port1.onmessage = t.unreached_func('Should not have delivered message');
    c.port2.close();
    c.port2.postMessage('TEST');
    setTimeout(t.step_func_done(), time_to_wait_for_messages);
  }, 'Message sent from closed port should not arrive.');

async_test(t => {
    const c = new MessageChannel();
    c.port1.onmessage = t.unreached_func('Should not have delivered message');
    c.port1.close();
    const c2 = new MessageChannel();
    c2.port1.onmessage = t.step_func(e => {
        e.ports[0].postMessage('TESTMSG');
        setTimeout(t.step_func_done(), time_to_wait_for_messages);
      });
    c2.port2.postMessage('TEST', [c.port2]);
  }, 'Message sent to closed port from transferred port should not arrive.');

async_test(t => {
    const c = new MessageChannel();
    let isClosed = false;
    c.port1.onmessage = t.step_func_done(e => {
        assert_true(isClosed);
        assert_equals(e.data, 'TEST');
      });
    c.port2.postMessage('TEST');
    c.port2.close();
    isClosed = true;
  }, 'Inflight messages should be delivered even when sending port is closed afterwards.');

async_test(t => {
    const c = new MessageChannel();
    c.port1.onmessage = t.step_func_done(e => {
        if (e.data == 'DONE') t.done();
        assert_equals(e.data, 'TEST');
        c.port1.close();
      });
    c.port2.postMessage('TEST');
    c.port2.postMessage('DONE');
  }, 'Close in onmessage should not cancel inflight messages.');

test(() => {
  const c1 = new MessageChannel();
  const c2 = new MessageChannel();
  c1.port1.close();
  assert_throws_dom("DataCloneError", () => c2.port1.postMessage(null, [c1.port1]));
  c2.port1.postMessage(null, [c1.port2]);
}, "close() detaches a MessagePort (but not the one its entangled with)");
