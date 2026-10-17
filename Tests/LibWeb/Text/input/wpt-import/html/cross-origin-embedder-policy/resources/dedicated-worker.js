self.onmessage = (e) => {
  fetch(e.data, {mode: 'no-cors'}).then(() => {
    self.postMessage('LOADED');
  }, () => {
    self.postMessage('FAILED');
  });
};
