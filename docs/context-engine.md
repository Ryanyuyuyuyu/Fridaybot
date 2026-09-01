# Friday context engine

Friday does not need to identify a person's true emotion to feel attentive.
It needs a small number of reliable context cues, confidence that decays over
time, and behaviour with memory.

## Privacy-first signal ladder

### Tier 0: StopWatch only

- Touch, buttons and BMI270 motion: direct attention and handling.
- Time since interaction: looking around, waiting and sleeping.
- Microphone energy features: silence, speech-like activity and keyboard-like
  bursts. Raw audio does not need to be stored or transmitted.
- RTC and charging state: working hours and whether Friday is docked at a desk.

### Tier 1: lightweight computer companion

The recommended first integration is a small local companion process that
sends coarse state over Bluetooth Low Energy. It should never transmit typed
content. Friday advertises slowly while disconnected, uses relaxed connection
parameters and accepts one four-byte event packet plus a low-rate heartbeat.

- Seconds since keyboard or mouse input.
- Screen locked, unlocked, asleep or awake.
- Input activity rate, reduced to `quiet`, `active` or `intense`.
- Pointer region, reduced to left, centre or right. The user calibrates which
  side of the monitor Friday occupies.
- Optional active-app category such as editor, browser, meeting or media. Do
  not transmit document names, URLs or window titles.

Mouse idle time alone is weak evidence: the user may be reading. Combine it
with lock state, recent keyboard activity and optional local sound energy.

### Tier 2: explicit opt-in local vision

A computer webcam can detect only `face_present` and `looking_toward_friday`.
All frames remain on the computer and only those two booleans are sent. This
is the closest approximation to reciprocal eye contact, but it is optional and
is not required for the first companion release.

## Context states

| State | Evidence | Friday behaviour |
| --- | --- | --- |
| `ATTENDING` | Touch, pickup, or optional gaze cue | Looks back, soft blink, petted response |
| `WORKING` | Continuous input and unlocked screen | Calm face; occasionally glances toward the work direction |
| `DEEP_FOCUS` | 20+ minutes of sustained activity | Quieter motion; occasional stretch or break reminder |
| `UNCERTAIN` | No input but screen awake | Looks around without assuming the user left |
| `AWAY` | Locked/asleep, or long idle plus silence | Short search sequence, then sleeps |
| `RETURNED` | First input/unlock after `AWAY` | Excited wake-up and acknowledgement |
| `MEETING` | Optional local mic/camera-in-use signal | Attentive but deliberately non-distracting |

## State inference

Use scores rather than hard replacement rules. Each observation adds evidence
and each score decays. State changes require a short dwell time, and leaving a
state uses a different threshold from entering it. This hysteresis prevents a
single mouse pause or desk vibration from making Friday change its mind.

The first host milestone sends only a compact equivalent of:

```text
presence=present activity=active pointer=right screen=unlocked
```

Friday owns the personality timing. The computer reports context; it does not
command individual animation frames.
