// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: timeout=long

'use strict';

// https://w3c.github.io/mediacapture-main/

idl_test(
  ['mediacapture-streams'],
  ['webidl', 'dom', 'html', 'permissions'],
  async idl_array => {
    const inputDevices = [];
    const outputDevices = [];
    try {
      const list = await navigator.mediaDevices.enumerateDevices();
      for (const device of list) {
        if (device.kind in self) {
          continue;
        }
        assert_in_array(device.kind, ['audioinput', 'videoinput', 'audiooutput']);
        self[device.kind] = device;
        if (device.kind.endsWith('input')) {
          inputDevices.push(device.kind);
        } else {
          outputDevices.push(device.kind);
        }
      }
    } catch (e) {}

    try {
      self.stream = await navigator.mediaDevices.getUserMedia({audio: true});
      self.track = stream.getTracks()[0];
      self.trackEvent = new MediaStreamTrackEvent("type", {
        track: track,
      });
    } catch (e) {}

    idl_array.add_objects({
      InputDeviceInfo: inputDevices,
      MediaStream: ['stream', 'new MediaStream()'],
      Navigator: ['navigator'],
      MediaDevices: ['navigator.mediaDevices'],
      MediaDeviceInfo: outputDevices,
      MediaStreamTrack: ['track'],
      MediaStreamTrackEvent: ['trackEvent'],
      OverconstrainedError: ['new OverconstrainedError("constraint")'],
    });
  }
);
