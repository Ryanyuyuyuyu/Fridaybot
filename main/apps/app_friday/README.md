# Friday face engine

Friday is the default companion app for the M5Stack StopWatch. Its face is
rendered from two persistent LVGL objects on a true-black AMOLED background;
there are no per-frame allocations or bitmap animation frames.

## Current interactions

- Tilt the device gently: after a short hold Friday peeks toward the lower
  bezel, using an asymmetric tall/short capsule pose rather than merely
  sliding two eyes around the display.
- Hold it at a steep angle: Friday braces against the apparent slope and pulls
  its eyes closer together. Turn it screen-down and it switches to a separate
  upside-down, confused wobble.
- Rock it gently from side to side several times: Friday recognises the
  alternating rhythm and joins in with a happy rocking pose. Shake it quickly
  back and forth instead and it becomes annoyed and asks to be left alone.
- Rotate it faster than roughly 145 degrees per second: Friday becomes dizzy
  and tracks a circular path.
- Brief low gravity while lifting or lightly tossing it: the eye capsules
  separate and float. A quick set-down or impact now plays a longer
  startled sequence: open, nervous asymmetric tremble, then recover.
- Pick it up after at least 6.5 seconds of stillness: Friday recognises the
  return of motion and gives a welcome response.
- IMU actions use independent entry/exit thresholds, short hold times, energy
  decay and reaction cooldowns. This makes small intentional gestures respond
  early without letting sensor noise flicker between emotions.
- Tap the screen: a quick surprised "boop" that opens and rebounds without the
  old forced squeeze.
- Hold for about half a second: Friday notices the contact, then settles into a
  soft petted/nuzzling pose. On release it becomes bashful, peeks back toward
  the touch and gradually returns over about 3.4 seconds.
- Drag slowly: the whole face follows the contact through bounded translation,
  asymmetric deformation and a springy release.
- Drag all the way to an edge: both eyes are pushed against the bezel and
  visibly compacted; release uses a lower-damping spring to jump past centre
  and settle back into place.
- Swipe quickly in any direction: Friday chases the swipe, overshoots, then
  springs back. Gesture direction is preserved, and a successful chase leaves
  Friday in a short proud pose instead of snapping straight to idle.
- Poke three times within roughly 1.8 seconds per poke: Friday becomes annoyed,
  looks away from the contact and gives a short dismissive twitch. Further
  pokes briefly sustain the mood instead of replaying surprise. When the anger
  passes it remains visibly sulky for several seconds.
- Touch reactions prioritize uninterrupted eye animation over haptic feedback
  because the current motor driver occupies the shared peripheral bus long
  enough to cause visible frame stalls.
- A/yellow button alone: no action.
- B/blue button: a mirrored upper-right bump turns into a game. Friday chases
  that corner, plays around it, feints toward centre, returns for a celebratory
  pair of alternating capsule hops and takes an elastic route home. The full
  sequence is about 4.7 seconds. The animation begins on the press edge
  and can be interrupted immediately by a new interaction. If uninterrupted,
  Friday stays delighted for a few seconds after the game.
- When the Mac companion is connected, drag Friday to the configured monitor
  edge and release to send it through the bezel. The exit pose completes before
  the eyes disappear. A visibility barrier then releases a matching circular
  Friday to peek around the Mac's outer display edge before sneaking in. Click
  it for a reaction, drag it through attached displays, or drag it back to its
  portal edge to return. StopWatch B recalls it. A second return barrier
  prevents the face from reappearing locally until the Mac panel is
  fully hidden. Missing acknowledgements or BLE disconnects restore it safely.
- Hold both buttons: return to the original M5Stack launcher.
- Leave it still: autonomous micro-saccades and blinking continue. Small
  curious/happy moments begin after roughly five to seven seconds. After ten
  seconds Friday can also perform an alternating playful hop that reads as an
  invitation to play, with an occasional lonely pose later; after 45 seconds
  Friday becomes sleepy.
- Six additional moods expand the vocabulary without changing the eye design:
  `daydreaming` drifts slowly during longer idle periods, `expectant` invites
  interaction, `proud` follows successful play, `sulky` follows annoyance,
  `suspicious` watches a questionable source, and `delighted` carries a
  celebration beyond its initial bounce.
- Expressions are moods rather than single frozen drawings. Idle, listening,
  thinking, curious, happy, playful, sleepy, shy, sad, annoyed and peeking each
  own a small pool of vertical-capsule poses. Friday changes pose on a
  mood-specific cadence, so a sustained state remains alive without looking
  like a looping sprite sheet.
- Lateral attention uses a pseudo-spherical head turn: the near eye subtly
  grows while the far eye compresses, their depth offsets change, and spacing
  contracts. The effect makes the black physical dial read as Friday's entire
  head instead of a pair of eyes moving inside a rectangular screen.
- Blink intervals are state-specific. Calm idle waits longer, playful and
  welcome states blink more eagerly, startled states blink sooner, and sleepy
  states keep a slower rhythm. The eye silhouette always remains a vertical
  capsule; no crescent or horizontal-line poses are used.
- Petting or picking it up after sleep produces a warm double-hop welcome
  instead of the normal petted response. A `returned` context triggers the
  same distinct "you're back" emotion.

## Optional Mac context

The local macOS helper sends only coarse presence and travel state to Friday.
Friday never receives keystrokes, pointer coordinates, window titles, URLs, or
camera data. When Friday lives on the Mac, the overlay uses the current pointer
position locally to direct its gaze; it does not retain or
transmit that position. The helper is an enhancement rather than a dependency:

- `working`: enters an attentive `listening` mood and occasionally glances
  toward the configured monitor;
- `uncertain`: alternates multi-pose `thinking` and `suspicious` moods while
  looking around without assuming that the user has left;
- `away`: searches briefly, then sleeps after six seconds;
- `returned`: gives an excited acknowledgement;
- `meeting`: remains in the quieter `listening` mood without soliciting play.

Packets are sent on state changes and as a 15-second heartbeat. A disconnected
or 45-second-stale link immediately restores the fully local behaviour. BLE
callbacks only validate and store a four-byte packet; expression selection and
rendering remain in the face loop.

See [`companion/macos/README.md`](../../../companion/macos/README.md) to build
and run the helper.

## Animation budget

- Behaviour and pose update: approximately 62.5 Hz (`16 ms`).
- BMI270 sampling: approximately 33 Hz (`30 ms`). The sampled target is
  interpolated by the 62.5 Hz spring, keeping motion continuous while leaving
  CPU/I²C headroom for rendering.
- LVGL display refresh: approximately 62.5 Hz (`16 ms`), reduced from the
  upstream 33 ms default.
- Blink: 280 ms, with a faster close and slower open.
- Motion is elapsed-time based and uses stable spring substeps, so a delayed
  frame changes neither animation duration nor the final pose.
- The black dial is the head coordinate system. Normal tilt translation is
  capped at seven pixels; direct touch and swipe may briefly travel farther,
  but expression is still communicated mainly through deformation around the
  bezel-anchored facial skeleton.

Run the host-side model checks with:

```sh
c++ -std=c++17 -Wall -Wextra -Werror \
  tests/face_model_test.cpp main/apps/app_friday/face_model.cpp \
  -o /tmp/friday_face_model_test
/tmp/friday_face_model_test

c++ -std=c++17 -Wall -Wextra -Werror \
  tests/context_protocol_test.cpp -o /tmp/friday_context_protocol_test
/tmp/friday_context_protocol_test

c++ -std=c++17 -Wall -Wextra -Werror \
  tests/presence_protocol_test.cpp -o /tmp/friday_presence_protocol_test
/tmp/friday_presence_protocol_test
```
