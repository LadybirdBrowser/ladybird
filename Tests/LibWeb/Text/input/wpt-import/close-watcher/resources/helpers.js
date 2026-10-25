window.createRecordingCloseWatcher = (t, events, name, type, parentWatcher) => {
  let watcher = null;
  if (type === 'dialog') {
    watcher = document.createElement('dialog');
    watcher.textContent = 'hello world';
    t.add_cleanup(() => watcher.remove());
    if (parentWatcher?.appendChild) {
      parentWatcher.appendChild(watcher);
    } else {
      document.body.appendChild(watcher);
    }
    watcher.showModal();
  } else if (type === 'popover') {
    watcher = document.createElement('div');
    watcher.setAttribute('popover', 'auto');
    watcher.textContent = 'hello world';
    t.add_cleanup(() => watcher.remove());
    if (parentWatcher?.appendChild) {
      parentWatcher.appendChild(watcher);
    } else {
      document.body.appendChild(watcher);
    }
    watcher.showPopover();
  } else {
    watcher = new CloseWatcher();
    t.add_cleanup(() => watcher.destroy());
  }

  const prefix = name === undefined ? '' : name + ' ';
  watcher.addEventListener('cancel', e => {
    const cancelable = e.cancelable ? '[cancelable=true]' : '[cancelable=false]';
    events.push(prefix + 'cancel' + cancelable);
  });
  watcher.addEventListener('close', () => {
    events.push(prefix + 'close');
  });

  return watcher;
};

window.createBlessedRecordingCloseWatcher = async (t, events, name, type, parentWatcher) => {
  await maybeTopLayerBless(parentWatcher);
  return createRecordingCloseWatcher(t, events, name, type, parentWatcher);
};

window.destroyCloseWatcher = (watcher) => {
  if (watcher instanceof HTMLElement) {
    watcher.remove();
  } else {
    watcher.destroy();
  }
};

window.sendEscKey = () => {
  // Esc is \uE00C, *not* \uu001B; see https://w3c.github.io/webdriver/#keyboard-actions.
  //
  // It's important to target document.body, and not any element that might stop receiving events
  // if a popover or dialog is making that element inert.
  return test_driver.send_keys(document.body, '\uE00C');
};

// For now, we always use the Esc keypress as our close request. In
// theory, in the future, we could add a WebDriver command or similar
// for the close request, which would allow different tests on platforms
// with different close requests. In that case, we'd update this
// function, but not update the sendEscKey function above.
window.sendCloseRequest = window.sendEscKey;

window.maybeTopLayerBless = (watcher) => {
  if (watcher instanceof HTMLElement) {
    return blessTopLayer(watcher);
  }
  return test_driver.bless();
};

window.waitForPotentialCloseEvent = () => {
  // CloseWatchers fire close events synchronously, but dialog elements wait
  // for a rAF before firing them.
  return new Promise(requestAnimationFrame);
};
