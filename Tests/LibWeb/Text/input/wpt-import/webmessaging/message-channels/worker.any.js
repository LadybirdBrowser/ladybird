async_test(t => {
  function workerCode() {
    close();
    var mc = new MessageChannel();
    mc.port1.postMessage(42);
    mc.port2.postMessage(42);
    postMessage(true);
  }

  var workerBlob = new Blob([workerCode.toString() + ";workerCode();"], {type:"application/javascript"});

  var w = new Worker(URL.createObjectURL(workerBlob));
  w.onmessage = function(e) {
    assert_true(e.data, "MessageChannel created on worker shutdown.");
    t.done();
  }
}, 'MessageChannel/MessagePort created and used after a worker self.close()');
