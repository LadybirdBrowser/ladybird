// Returns a reference to a worklet object corresponding to a given type.
function get_worklet(type) {
  if (type == 'animation')
    return CSS.animationWorklet;
  if (type == 'layout')
    return CSS.layoutWorklet;
  if (type == 'paint')
    return CSS.paintWorklet;
  if (type == 'audio')
    return new OfflineAudioContext(2,44100*40,44100).audioWorklet;
  return undefined;
}
