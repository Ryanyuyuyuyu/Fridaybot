# Friday cross-device presence

## Goal

Friday should feel like one persistent character that can move between the
StopWatch, a Mac and every display attached to that Mac. It must not become a
video-streaming feature or make the offline face depend on a computer.

## Core model

Only one device owns Friday at a time. A transfer sends a compact state packet,
not rendered frames:

- character/session identifier;
- source and destination device;
- expression and pose variant;
- animation phase and deterministic random seed;
- handoff direction and synchronized start time;
- short ownership lease and sequence number.

Each device runs the same 60 FPS-class face behaviour locally. The StopWatch
therefore keeps its current low-cost animation loop, while the Mac can render a
matching circular black Friday in a transparent overlay.

## Transport

- StopWatch to nearby Mac: the existing BLE companion connection;
- displays attached to one Mac: local process state, one overlay per display;
- additional Macs: Bonjour discovery plus an authenticated local-network
  channel in a later phase;
- no cloud account or internet connection is required for the first version.

## Handoff sequence

1. The source reserves the transfer and keeps ownership during the exit pose.
2. Friday compresses against the chosen edge and disappears through it.
3. The destination acknowledges that it is ready but remains invisible.
4. The source hides its final frame and publishes a visibility barrier; only
   then may the destination draw Friday's first frame.
5. Return uses the same rule in reverse: the destination announces that it is
   leaving, fully hides its panel, then releases the source to draw.
6. Sequence validation, idempotent retries and a timeout ensure Friday can be
   neither duplicated nor lost between devices.

The companion will provide a small desk map describing whether the StopWatch
and displays are left, right, above or below one another. This evolves the
current `workDirection` setting into a spatial device graph.

## First prototype (implemented)

The current demonstration moves ownership from StopWatch to one Mac and back:

- an edge drag on StopWatch sends Friday toward the configured Mac side;
- a circular Friday overlay emerges from the corresponding Mac screen edge;
- the Mac renders idle, cursor-awareness and local expressions itself;
- clicking the Mac Friday plays locally; dragging it to its portal edge or
  pressing a StopWatch hardware button returns it;
- disconnecting BLE cancels a pending transfer and restores the normal local
  StopWatch personality.

The transfer uses deterministic seeds, a local 60 FPS Mac renderer, two-phase
visibility barriers, acknowledgement timeouts and disconnect recovery. A
single movable panel selects the outermost `NSScreen` as its entry portal and
can be dragged through every display attached to that Mac without creating a
second face. The BLE link temporarily switches from its relaxed office interval
to a responsive interval during the handoff, then immediately returns to the
low-duty setting. The next step is a saved desk map, followed by Bonjour
discovery for additional Macs.
