# Concurrent gray marking in V8

## Why

The cycle collector needs gray bits: the set of JS objects reachable only from gray roots
(JS holders of native objects), not from black roots. Until now V8 computed this set inside
the atomic pause, after the black fixpoint, by marking from the gray roots with recording on.
On the game that is 40K objects and 80K edges, 1.5–4 ms of the 9–15 ms pause, plus the cell
snapshot. Chrome's whole pause on the same game is 3.5 ms; its marking is concurrent.

## Reference: SpiderMonkey

- Two mark bits per cell: black and gray-or-black.
- Two mark stacks; black is always drained before gray (`doMarking`, `markOneColor`).
- Gray roots are marked incrementally in their own phase after black marking of the group.
- Black marking that reaches a gray cell marks it black and traces it again.
- Barriers mark black. `ExposeGCThingToActiveJS` unmarks gray recursively outside marking and
  performs the read barrier during marking.
- Weak map values take the lesser color of map and key.

## Mapping onto V8

- Mark bit keeps its meaning: marked = live (black or gray). All of V8 that asks "is it live"
  is untouched.
- Gray is a second bit in a separate per-page gray bitmap, allocated on demand during the
  gray phase. A separate bitmap, not the second bit of the marking bitmap, because black
  allocation sets whole bit ranges.
- `MarkingWorklists::Local` carries the color it is marking with. In gray color, pushes go to
  the gray worklist and a successful mark sets the gray bit first, then the mark bit.
- Black `TryMarkAndPush` that finds an object marked and gray clears the gray bit (one winner)
  and pushes it to the reblack worklist. Reblack visits do not account live bytes again and
  reset a large object's progress bar.
- Every marker thread drains black, then reblack, then gray. Gray visits record the object and
  its edges.
- Ephemeron values are marked with the lesser color of table and key.
- Marking barrier: the C++ barrier blackens a gray value; the RecordWrite builtin takes the slow
  path for a marked value whose gray bit is set.
- Gray phase starts when incremental marking would finalize for the first time in the cycle:
  the embedder marks gray roots, concurrent marking continues; finalize when both colors are
  drained.
- Atomic pause: black roots and black fixpoint (blackening as above), gray roots again for
  holders added since, gray fixpoint, then the result: recorded objects whose gray bit is still
  set, recorded edges, weak candidates.
- Wrappers: cppgc has no color, so a DOM wrapper's anchor is traced by V8 itself through an
  embedder callback with the visitor's color, and V8 marks the anchor in cppgc without pushing
  it for tracing.

## Embedder side (Bedrock jsapi)

- `CellGrayMarkingClient::TraceWrapper` traces an anchored cell's slots, class trace hook and
  proxy handler with a marker that marks each target's traced reference in the given color.
- Gray graph is built off the pause (already in place); read barrier unmarks gray on apply.
