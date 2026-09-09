# Friday requirements backlog

This document records product ideas that are intentionally not part of the
current implementation milestone. An item marked `Deferred` must not start
development until it is explicitly promoted.

## FR-PLATFORM-001 — Friday on iPhone and personal-device travel

- **Status:** Deferred — requirements only, do not implement now
- **Added:** 2026-09-02
- **Target devices:** M5Stack StopWatch, Mac, and the owner's two iPhones

### Product intent

Friday should feel like one persistent character that can travel wirelessly
between all of the owner's nearby devices. Moving to an iPhone must preserve
Friday's identity, mood and continuity rather than launching an unrelated
mobile copy.

### Desired experience

- Pair trusted personal devices into a small private device graph.
- Let the user send Friday from the StopWatch or Mac toward a selected iPhone,
  with direction-aware exit and arrival animation.
- Render the complete interactive Friday inside a dedicated iPhone app while
  that app is in the foreground.
- When the app leaves the foreground, let Friday visibly retreat into an
  iOS-supported surface such as a Live Activity, Lock Screen presentation or
  Dynamic Island presentation when available; do not imply that iOS supports
  a free-floating character above arbitrary apps.
- Restore the full animation coherently when the user opens the Friday app.
- Support two paired iPhones without duplicating Friday or making destination
  selection ambiguous.
- Preserve direct touch, autonomous moods and device-edge contact as much as
  each iOS presentation permits.
- Require no wired connection for normal use.

### Continuity and safety requirements

- Exactly one device owns Friday's visible full-body presence at a time.
- Reuse or extend the current sequence, lease, acknowledgement and two-way
  visibility-barrier model so Friday can be neither duplicated nor lost.
- A failed handoff, unavailable destination, app termination or radio loss
  must restore Friday to the last confirmed owner or safely return it to the
  StopWatch.
- Pairing and device-to-device traffic must be authenticated and limited to
  devices approved by the owner.
- Background operation must respect iOS execution, battery and privacy rules.

### Likely technical direction (not yet committed)

- Use BLE for nearby StopWatch-to-gateway discovery and compact handoff data.
- Keep one active StopWatch companion connection instead of requiring the
  embedded device to maintain simultaneous links to the Mac and both phones.
- Let trusted Apple devices relay ownership over an authenticated local-network
  channel; consider an optional encrypted cloud relay only for travel outside
  the local network.
- Share a platform-neutral character state and animation vocabulary while each
  device renders locally.

### Explicitly out of scope for the first mobile milestone

- A permanent free-floating iOS overlay above every third-party app.
- Continuous full-frame animation while the iPhone screen is off.
- Unattended internet-scale roaming or a mandatory cloud account.
- Android support; evaluate it as a separate platform requirement.

### Open decisions before development

- Exact iPhone models and supported iOS versions.
- Whether the first prototype uses direct StopWatch-to-iPhone BLE or routes
  through the Mac.
- How a user selects one of two nearby iPhones as the destination.
- Which transitions should be automatic versus explicitly initiated.
- App distribution, signing and whether App Store release is required.
- The reduced visual language for Dynamic Island, Lock Screen and Live Activity.

### Promotion gate

Begin implementation only after the Mac travel and autonomous desktop behaviour
have been validated, the target iPhones are identified, and the user explicitly
asks to resume this requirement.
