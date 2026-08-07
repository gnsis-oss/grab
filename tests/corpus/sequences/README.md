# Sequence JSON corpus

Real-world and adversarial documents for the sequence interpreter, per
`workspace/CLAUDE.md` §6.1: anything consuming external bytes ships a corpus and
is compared against a canonical tool, because pure unit tests over synthesised
inputs miss spec-versus-implementation gaps.

**The assertion is NO CRASH AND NO SANITIZER TRIP — not "no rejection".** An
adversarial corpus earns its place precisely by pushing the parser. A file under
`invalid/` is *expected* to be rejected; a clean rejection with a useful message
is a pass. A segfault, an abort, an ASan/UBSan report, or an unbounded run is a
failure whichever directory the file sits in.

`valid/` and `invalid/` hold data files only; the harness that runs them is
`tests/corpus/run_sequence_corpus.sh`, label `sequence_corpus`. Adding a `.json`
file to either directory registers a test with no CMake edit — the `add_test`
glob at `tests/CMakeLists.txt:355-386` is deliberate — so re-run `cmake` after
adding one.

## The harness, and what it actually asserts

`run_sequence_corpus.sh <grab-binary> <document.json> expect-valid|expect-rejected`
runs `grab play <document> --dry-run`, which opens no seat, needs no display and
executes nothing. It prints diagnostics and then exactly one verdict token as
the last line, and ctest matches on that token with `PASS_REGULAR_EXPRESSION` —
**so the exit status is advisory and the token is the verdict.**

It prints **no token at all** for any of these, which is how "no crash" becomes
an assertion rather than a hope:

| Condition | Why it is not a verdict |
|---|---|
| death by signal (exit ≥ 128) | a crash fails whichever directory the file is in |
| exceeding the 60 s timeout | an unbounded run is a failure, per the rule above |
| `AddressSanitizer` / `ThreadSanitizer` / `runtime error:` / `SUMMARY: …Sanitizer` in either stream | **ASan's default exit code is 1, exactly grab's "document refused"** — without a separate scan for the report text, every ASan trip on an `invalid/` file would read as a clean rejection and pass |
| exit 2 | a command-line error is a harness bug, not a verdict on the document |
| exit 0 with no `sequence:`/`steps: N`/`order:`/`plan:` line | the `--dry-run` plan contract changed underneath the corpus |
| exit 1 with an empty stderr, or with no JSON pointer in the message | a refusal carrying no diagnostic is indistinguishable from a silent death |

For an accepted document it additionally replays the file under `--pacing
strict`, `grace` and `precise` and asserts the `order:` line is **byte-identical
across all three** while `plan:` never decreases. That is design §4.10's
invariant — the mode governs timing and nothing else — and it is the assertion
that proves `extra-grace-under-strict.json` is *ignored* rather than rejected:
the same document plans 10 ms under `strict` and 66 745 ms under `precise`.

**Never write either verdict token into a diagnostic line of that script.** The
`invalid/` tests pass on seeing `[REJECTED]` anywhere in the output, so a
diagnostic that quoted the token would turn a real failure green. The script
emits tokens through exactly one function, `print_verdict`, for that reason.

---

## Contents at a glance

| Directory | Files | Expected verdict |
|---|---|---|
| `valid/` | 15 | `[VALID]` — every one must load |
| `invalid/` | 35 | `[REJECTED]` — every one must be refused |

50 files, 50 registered tests. The count was written as 16 before the harness
existed and counted the withdrawn `too-many-steps.json`, which is described
below but deliberately not vendored. It then read 22 for as long as the corpus
covered input, capture and wait only. On 2026-08-06 the eight `overlay.*` ops of
design §3.2 added 7 valid and 20 invalid documents, and
`remaining-payload-spellings.json` closed the five non-overlay gaps this file
used to list at the bottom.

---

## `valid/` — each of these MUST be accepted

| File | Probes | Notes |
|---|---|---|
| `spec-example.json` | The `login-flow` document from design §3, **verbatim**. | The reference document. It is the only file that spells a button as a string (`"button": "left"`), because the spec does; if the interpreter cannot load this file it does not implement the spec. Also the file the Phase 3 headless smoke test plays. |
| `linear-500.json` | 500 steps with **no `after` anywhere** — the implicit depends-on-the-previous-step edge at scale. | Also has **no `pacing` key at all**, so it probes `strict` being the documented default rather than a required field. Every tenth `input.click` carries a label; the rest are unlabelled, proving an unlabelled step still receives a `StepId` and that the implicit edge points at whatever came before, labelled or not. Generated; one step per line. |
| `diamond.json` | Fork/join: `a → b → d` and `a → c → d`. | All four edges are explicit, so nothing depends on the implicit rule. `d` names two predecessors, which is the fan-in case. |
| `all-sequence-ops.json` | One step per **sequence-capable** op. | There are exactly **23**: the descriptor table names **38** commands and 23 of them have a payload struct in the `Command` variant at `include/grab/command.hpp:251-273`, which `static_assert`s its own size against `sequenceCommandCount`. Verified against the table rather than from memory — the 23 op strings here are exactly the complement of the 15 rejected by `is_sequence_command()`. **The name was a lie between the overlay ops landing and 2026-08-06**, when the file still carried 15. `press` is paired with `release` and `key_down` with `key_up`, because the caller owns what it presses; the overlay handle `marker` is added, updated, attached, detached and removed in that order, because the loader tracks handle liveness in document order. |
| `unicode-type.json` | `input.type` payloads across the UTF-8 and JSON-escape space. | 16 steps: ASCII, 2-byte Latin-1 supplement, Greek/Cyrillic, 3-byte CJK, NFC precomposed vs **genuinely NFD decomposed** (`65 cc 81`, not `c3 a9`), four stacked combining marks on one base, 4-byte emoji with a skin-tone modifier, a ZWJ family, regional-indicator flags, Arabic and Hebrew RTL runs, a mixed-direction run, an explicit U+202E/U+202C bidi override, raw ZWSP/ZWJ/BOM, JSON `\t \n \\ \"` escapes, BMP `\uXXXX` escapes, and a **surrogate-pair** escape (`👍🏽`). |
| `extra-grace-under-strict.json` | `"mode": "strict"` **with** `extra_grace_ms` present on every step. | **This must LOAD AND BE IGNORED, never rejected.** Design §4.10 makes the mode the sole authority; rejecting the field would defeat the whole point of running one document under all three pacing modes without editing it. The largest value (65535 ms) is deliberately absurd — if it were honoured under `strict` the run would visibly stall. |
| `chord-ctrl-c.json` | `key_down Control_L` → `key c` → `key_up Control_L`. | The case that was unreachable before `key_down`/`key_up` entered the table: `Keystroke` carries only shift and altgr, so Ctrl+C cannot be expressed through `type`/`key` alone. |
| `remaining-payload-spellings.json` | Every spelling the "still not exercised" table below used to list. | `on_error` in all three forms (`abort`, `continue`, `goto:recover`) — **there is no `on_error_target` key**, the target rides inside the string; `screen.capture` by `locator` as well as by `out`; `options.steps` / `options.step_dwell_ms` / `options.path` on `drag`, `move` **and** `follow`; non-default buttons as a name (`middle`, `right`), as an **alias** (`secondary`, which `to_json` writes back as `right`), and as a bare **integer** wheel code (`4`); and `time.wait` by `ns` beside `ms`. Runs under `"mode": "precise"`, so its `extra_grace_ms` is live rather than ignored. |

### Overlay steps — design §3.2

| File | Probes | Notes |
|---|---|---|
| `overlay-geometry.json` | **Every `Geometry` alternative**, one step each. | `rect`; `ellipse` in **both** spellings — the `radius` shorthand and explicit `radius_x`/`radius_y`; `polygon` at the three-point minimum; `path` as the bare command array of §3.2 including the bare string `"close"`, the one path command with no operand. Also a single-control `bezier` (the floor is **one** control point, not three), fractional coordinates — a `Shape` is `SpacePoint`, i.e. `double`, unlike the integer device points every `input.*` op carries — and a zero-extent rect, which is legal and invisible because only *negative* extents are refused. The last step uses the **object** path form `{ "commands": […], "closed": true }`, which §3.2 never mentions: the bare array cannot carry `Path::closed`, so `to_json` would otherwise be unable to write a value its own type holds (`interpreter.cpp:1325-1331`). |
| `overlay-styling.json` | `stroke`, `fill`, colour spelling, `band` and `z`. | Both colour forms — `#rrggbb` and `#rrggbbaa`; stroke-only, fill-only, both, and **neither**, which §3.2 calls legal and invisible and which the loader agrees with. Also a defaulted stroke width, a zero stroke width, a fully transparent `#00000000` fill, `band` in both spellings, a negative `z`, and an **uppercase** `#4ECEA9` — accepted, because `hex_digit` takes `A-F`, but note `color_text` always writes lowercase, so uppercase round-trips in value and not in bytes. |
| `overlay-lifetimes.json` | All three lifetimes. | Absent (defaults to persistent), the explicit string `"persistent"`, `{ "ttl_ms": N }` and `{ "fade_ms": N }`, plus `ttl_ms: 0`. It then waits past the ttl and removes the expired handle, which §3.2 says succeeds as a no-op — and which the **loader** accepts for a different reason than the runtime does: liveness is tracked structurally, so an unexpired-in-the-document handle is simply still live to the loader. |
| `overlay-animation-channels.json` | **Each of the four `AnimationSpec` channels**, alone and together. | `scale` (`from`/`to`), `opacity` (`from`/`to`), `translate` (`dx`/`dy`), `reveal` (`axis`, `from_edge`, `from`, `to`), each with a non-default `easing`; then one shape carrying all four at once; then a channel with **nothing but `duration_ms`**, proving every other field defaults. `duration_ms: 0` is accepted — the field is *required*, not *nonzero*, which is worth knowing given the comment at `interpreter.cpp:1566-1568` about no duration defaulting to zero. |
| `overlay-handles.json` | Handle liveness across a whole document. | Two `overlay.add` steps with **no handle at all** — fire-and-forget, drawable, never referenced, and not colliding with each other; two named handles live at the same time; an update on each; and a handle **reused after its remove**, which is legal precisely because `track_handles` erases it on remove (`interpreter.cpp:2577-2582`). |
| `overlay-attach-detach.json` | `overlay.attach` / `overlay.detach`, with and without `offset`. | An implicit-offset pair — the shape keeps the gap it already has, so a square picked up by its corner stays held by that corner — and an explicit `offset: [-24, -24]`, plus an explicit `[0, 0]`. `offset` is a `geometry::Point`, **integer device coordinates**, while everything inside `shape` is a `double`; §3.2 flags that split as inconsistent and unintended, and `invalid/overlay-fractional-attach-offset.json` pins it. |
| `overlay-annotated-drag.json` | A document mixing input and overlay steps the way a real one would. | Highlight the source and target, settle, press, **attach the highlight so it rides the pointer through `input.move`**, detach, release, restyle the source marker to mark the landing, drop a `trail`-band crumb with a `ttl_ms`, capture, then remove and clear. Runs under `"mode": "grace"`, carries an explicit `after` fan-out and fan-in, and is the only overlay file that exercises `options` on a move. This is the case §3.2 opens with: before these ops a sequence could move the pointer to a target and click it but could not place, move or remove the target. |

## `invalid/` — each MUST be rejected, and each probes a different failure

### Schema and graph rejections (well-formed JSON)

| File | Probes | Notes |
|---|---|---|
| `cycle-3.json` | `a → b → c → a`. | Requires the loader to resolve `after` labels in **two passes**: `a` names `c`, which is declared later. A single-pass loader rejects this as a dangling `after` instead — still `[REJECTED]`, so the harness passes either way, but the error message tells you which happened. |
| `self-edge.json` | A step whose `after` names itself. | **The subtle one.** `AdjacencyGraph::add_edge` returns `false` for a self-loop and the edge never enters the graph, so a topological sort *cannot see it*. If the loader does not treat that `false` return as an error, this file loads as VALID with its dependency silently dropped. That is a wrong-answer bug, not a crash, so only this file catches it. |
| `dangling-after.json` | `after` naming a label that does not exist. | Error must name both ends and carry a JSON pointer. |
| `duplicate-after.json` | The same predecessor listed twice in one `after`. | `add_edge` also returns `false` for a duplicate edge. A repeated entry is an error, not a no-op. |
| `duplicate-label.json` | Two steps sharing an `id`. | A third step names the ambiguous label, so the failure is a real unresolvability rather than mere bad style. |
| `unknown-op.json` | `"op": "input.telekinesis"`. | Must fail `command_kind()` lookup and name the op. |
| `not-a-sequence-op.json` | `"op": "system.doctor"`. | Resolves in the descriptor table but has **no payload struct**. Must produce a **different** message from `unknown-op.json` — *"op X is not available as a sequence step"* versus *"unknown op X"*. Two different author mistakes. Comparing these two files' stderr is the assertion. |
| `unknown-pacing-mode.json` | `"mode": "sloppy"`. | Not in `pacingModeNames`. |
| `negative-grace.json` | `grace_ms` below zero. | |
| `negative-extra-grace.json` | `extra_grace_ms` below zero, under `precise` so the field is live. | |
| *(withdrawn)* `too-many-steps.json` | **65537 steps — one past capacity.** | **Deliberately not vendored.** Capacity is **65536, not 65535**: `StepId`'s generation starts at 1, so index 0 is usable and a 16-bit index addresses 65536 slots (`maxSteps` in `sequence_types.hpp`). The file was generated at 1.9 MB / 65 544 lines, which is permanent repository weight carrying one bit of information. The same bound is asserted by a unit test that costs nothing — `Sequence.build` rejects an over-`maxSteps` document in `tests/kernel/test_sequence.cpp`. If the corpus harness ever wants the end-to-end path, generate the file into the build tree at test time rather than committing it. |

### Overlay rejections (design §3.2, well-formed JSON)

Every one of these was **run against the binary before the file was written**.
The implementer added five rules §3.2 does not state — negative extents
refused, polygons of at least three points, `color` required inside a stroke or
fill that is present at all, `duration_ms` required on every animation channel,
and `radius` mutually exclusive with `radius_x`/`radius_y` — and all five are
genuinely enforced, which is why each has a file rather than a footnote.

| File | Probes | Notes |
|---|---|---|
| `overlay-two-geometry-keys.json` | `rect` **and** `ellipse` in one shape. | A shape is one `Geometry` alternative. The message names *which* keys were found, not merely that the shape is invalid. |
| `overlay-no-geometry-key.json` | A shape with stroke, fill, lifetime, band and z — and **no geometry at all**. | The complement of the file above and the other half of the same `present != 1U` check; the message ends "found none". A styling-only object is the shape an author gets by deleting one line. |
| `overlay-malformed-color.json` | `#gg0000` — right length, right prefix, not hex. | Colour takes `#rrggbb` and `#rrggbbaa` and nothing else: `#fff` and a bare `4ecea9` are refused too, both verified. Uppercase **is** accepted — see `valid/overlay-styling.json`. |
| `overlay-radius-and-radius-x.json` | `radius` mixed with `radius_x`/`radius_y`. | §3.2 says `radius` is shorthand for equal radii but never says what mixing them means. The loader refuses rather than picking a precedence, which is the right call: which one wins is exactly what nobody should have to remember. |
| `overlay-negative-radius.json` | `radius: -48`. | `require_extent`, reached through the ellipse. |
| `overlay-negative-rect-extent.json` | `w: -90`, with a **negative `x`/`y` in the preceding step to prove position is not an extent**. | Same `require_extent` as above, reached through the rect. The two files share a rule and not a path; keeping both is what makes the shared rule visible. |
| `overlay-negative-stroke-width.json` | `width: -3`. | A **different** enforcement site: `read_stroke` checks the sign itself (`interpreter.cpp:1471-1476`) rather than going through `require_extent`, because width is optional and defaulted. |
| `overlay-polygon-two-points.json` | A polygon of two points, after a legal three-point one. | Three is the floor. §3.2's own example is a triangle and says nothing about a minimum. |
| `overlay-bezier-no-controls.json` | `{"bezier": []}`. | The floor is **one** control point, not three — `BezierTo::control` is a vector and the degree follows its length, exactly as `input.follow`'s `curve` does. |
| `overlay-stroke-without-color.json` | A `stroke` carrying only a `width`, after a shape with no stroke at all. | Absent stroke is legal; a *present* stroke needs a colour. The paired steps are what separate the two rules. |
| `overlay-animation-without-duration.json` | An `opacity` channel with `easing`, `from` and `to` but no `duration_ms`. | Required on every channel. Note `duration_ms: 0` **is** accepted, so this rule is "present", not "nonzero". |
| `overlay-handle-before-add.json` | `overlay.update` naming a handle whose `overlay.add` comes later in the document. | Handles resolve in **document order**, not through the label pre-pass that `after` uses: an `overlay.add` is what creates one, so naming it earlier is a use-before-creation rather than a forward reference. `remove`, `attach` and `detach` share the check and were each verified against the binary; the file leads with `update` because a document only fails on its first fault. |
| `overlay-duplicate-live-handle.json` | The same handle added twice while still live. | §3.2 makes this a loader error, and it is. Reuse *after* a remove is legal — see `valid/overlay-handles.json`. |
| `overlay-remove-after-remove.json` | Two `overlay.remove` steps on one handle. | A different author mistake from the file above: the handle really was created. **The message is misleading** — it reads "uses handle 'marker' before any overlay.add creates it", because `track_handles` erases the name on remove and cannot then tell "never existed" from "already retired". A rejection is right; the diagnostic is not, and this file is the one that shows it. |
| `overlay-unknown-path-command.json` | `{"arc": [...]}` inside a path. | An unrecognised key means the object carries **zero** of `move`, `line` and `bezier`, so it is caught by the exactly-one rule rather than by a name lookup. |
| `overlay-unknown-bare-path-command.json` | The bare string `"open"`. | The other half of `read_path_command`, and a genuinely different message: `"close"` is the only bare-string command. Two files because the object form and the string form fail through different branches. |
| `overlay-lifetime-both-forms.json` | `ttl_ms` **and** `fade_ms` in one lifetime. | An exactly-one rule §3.2 does not state, distinct from the geometry one, with its own message naming all three accepted lifetime forms. |
| `overlay-unknown-easing.json` | `"easing": "bouncy"`. | Pins `optional_enum`, the shared reader behind `band`, `easing`, `axis` and `from_edge`: an unknown name is refused **naming the whole accepted set**, never silently defaulted, because a defaulted easing looks exactly like a working one. |
| `overlay-empty-handle.json` | `"handle": ""`. | An absent handle is fire-and-forget and legal; a present-but-blank one is a typo. The two are one line apart in a document and one branch apart in `optional_handle`. |
| `overlay-fractional-attach-offset.json` | `"offset": [-24.5, -24.5]` on a shape whose own coordinates are fractional. | The `Point`-versus-`SpacePoint` split §3.2 flags as unintended, made visible: the same document spells `200.5` inside `shape` and is refused for spelling `-24.5` inside `offset`. If that split is ever closed, this file moves to `valid/`. |

### Parser rejections (the JSON parser's own error path)

These exist so the corpus exercises `nlohmann::json`'s failure path and the way
the interpreter surfaces it, not only the schema validator downstream of a
successful parse.

| File | Probes | Well-formed JSON? |
|---|---|---|
| `malformed-truncated-object.json` | Byte-truncated mid-array, unterminated. | **No** |
| `malformed-trailing-comma.json` | Trailing comma after the last array element — legal in JSON5, not in JSON. | **No** |
| `malformed-unquoted-key.json` | Bare identifier keys. | **No** |
| `malformed-root-array.json` | Wrong root type: a JSON **array** whose sole element is an otherwise-fine document. | **Yes** — see below |
| `malformed-empty.json` | Zero-length file. | **No** — see below |

---

## Canonical-tool comparison: two known `jq` disagreements

CLAUDE.md §6.1 step 3 requires comparing against a canonical tool and
investigating every disagreement. Two are known and are **not bugs**; the
harness must not treat either as one.

The full 50-file matrix — every file's verdict from both tools, the reasoning
for every disagreement, the ASan run, and what round-tripping is and is not
covered — is recorded in
`workspace/findings/2026-08-06-sequence-corpus-comparison.md`. The short version:
the only direction that would be a bug is *`jq` rejects and grab accepts*, and
it never occurs; the harness makes that direction a hard failure and reports the
reverse.

**1. `jq empty` exits 0 on a zero-length file.** `jq` reads zero inputs and
therefore has nothing to complain about:

```
$ jq empty invalid/malformed-empty.json ; echo $?
0
$ jq -e . invalid/malformed-empty.json >/dev/null 2>&1 ; echo $?
4
```

`jq empty` is the wrong well-formedness oracle for this corpus. Use `jq -e .`,
or test for a zero-length file first. `malformed-empty.json` must still be
`[REJECTED]` by grab.

**2. `malformed-root-array.json` is well-formed JSON that grab must reject.**
This is a *root-type* rejection, not a parse error, so `jq` and grab legitimately
disagree: `jq` parses it, grab refuses it because a sequence document's root is
an object. It carries the `malformed-` prefix because it belongs to the
wrong-root-type group named in the corpus requirement, not because it is
syntactically broken. Documented here so the disagreement is closed rather than
re-investigated.

Verification command, and the expected split:

```bash
cd tests/corpus/sequences
for f in valid/*.json;   do jq -e . "$f" >/dev/null || echo "BAD $f"; done   # must print nothing
for f in invalid/*.json; do jq -e . "$f" >/dev/null 2>&1 || echo "not-json: $f"; done
#   not-json: invalid/malformed-empty.json
#   not-json: invalid/malformed-trailing-comma.json
#   not-json: invalid/malformed-truncated-object.json
#   not-json: invalid/malformed-unquoted-key.json
# every other invalid/ file is well-formed JSON and fails on schema or graph rules
```

**The 20 overlay `invalid/` files add no third disagreement.** Every one of them
is well-formed JSON that grab refuses on a §3.2 shape rule, so all 20 land in
the same "higher-level rejection" bucket as `self-edge.json` and
`unknown-op.json` already did. The split above is therefore unchanged by them:
still exactly four files that are not JSON at all.

---

## The grammar these files assume

The document shape is pinned by design §3, and every payload key by design
§3.1. **Nothing here is inferred any more.** This section previously marked
seven ops as guesses from the payload struct members in
`include/grab/command.hpp`; §3.1 subsequently pinned all of them — member name
verbatim, with one exception — and the interpreter implements exactly that
(`src/kernel/sequence/interpreter.cpp:74-104` is the input key table,
`:116-152` the overlay one). All 50 files load or reject as intended, so the
reconciliation is closed.

Document level:

| Key | Type | Source |
|---|---|---|
| `schema_version` | integer | spec §3 |
| `sequence` | string, the document name | spec §3 |
| `pacing.mode` | `"strict"` \| `"grace"` \| `"precise"`, default `strict` | spec §3, §4.10 |
| `pacing.grace_ms` | integer ≥ 0 | spec §3 |
| `steps` | array | spec §3 |

Step level:

| Key | Meaning | Source |
|---|---|---|
| `id` | optional **author label**, not the identity | spec §3, §9.3 |
| `op` | one of the **38** descriptor-table names; only the **23** with a payload struct are accepted | spec §5.2 |
| `after` | array of **labels**, or bare document indices; omitted means depends-on-the-previous-step | spec §3 |
| `on_error` | `"abort"` (default) \| `"continue"` \| `"goto:<label>"` | spec §3.1, §7 |
| `extra_grace_ms` | integer ≥ 0, read only under `precise` | spec §3, §4.10 |
| `handle` | overlay steps only: a **document-level name**, like a label, never serialized as a `ShapeId` | spec §3.2 |

Payloads:

| Op | Fields used here | Source |
|---|---|---|
| `input.move` | `to: [x, y]` | spec §3 |
| `time.wait` | `ms` | spec §3 |
| `input.click` | `button` (`"left"` \| `"middle"` \| `"right"`, or an integer 1–7) or omitted for the default | spec §3, §3.1 |
| `screen.capture` | `out` | spec §3 |
| `input.type` | `text` | spec §3 |
| `input.warp` | `to: [x, y]` | spec §3.1 — member name verbatim |
| `input.click_at` | `at: [x, y]` | spec §3.1 — member name verbatim |
| `input.press` / `input.release` | none here (default button); `button` is accepted | spec §3.1 |
| `input.scroll` | `dx`, `dy` (notches; +dy down, +dx right) | spec §3.1 — member names verbatim |
| `input.drag` | `from: [x, y]`, `to: [x, y]` | spec §3.1 — member names verbatim |
| `input.key` / `input.key_down` / `input.key_up` | `key` | spec §3.1 — member name verbatim |
| `input.follow` | `curve: [[x, y], ...]` — the control points, degree implied by length | spec §3.1, **and this is the one exception to the member-name rule**: the member is `path`, but `path` is already `DragOptions::path`, the curve *kind* (`"linear"` / `"cubic"`) inside `options`. One key cannot be both an array of points and a kind string, so the point list is `curve`. `all-sequence-ops.json` uses `curve` and loads. |
| `screen.capture` | exactly one of `out` and `locator` — never both, never neither | spec §3.1 |
| `time.wait` | exactly one of `ms` and `ns` | spec §3.1 |
| `input.drag` / `input.move` / `input.follow` | `options: { steps, step_dwell_ms, path }` — **not** `interpolation_steps`/`step_dwell` | spec §3.1 |

Overlay payloads, design §3.2:

| Op | Fields used here | Notes |
|---|---|---|
| `overlay.add` | `handle?`, `shape` | An absent handle is fire-and-forget; a present-but-empty one is rejected. |
| `overlay.update` | `handle`, `shape` | Both required. |
| `overlay.remove` / `overlay.attach` / `overlay.detach` | `handle` (+ `offset?` on attach) | `offset` is `[x, y]` in **integer** device coordinates, unlike every coordinate inside `shape`. |
| `overlay.clear` / `overlay.grab` / `overlay.release` | — | No payload. Extra keys on these steps are ignored, like extra keys everywhere else. |
| `shape.rect` | `x`, `y`, `w`, `h` — `w`/`h` may not be negative | |
| `shape.ellipse` | `center: [x, y]` plus **either** `radius` **or** both `radius_x` and `radius_y` | Mixing the two spellings is an error, not a precedence rule. |
| `shape.polygon` | `[[x, y], …]`, at least three points | |
| `shape.path` | `[ {"move": [x,y]}, {"line": [x,y]}, {"bezier": [[x,y], …]}, "close" ]` | `bezier` needs at least one control point. The object form `{ "commands": […], "closed": bool }` also loads, and is the only spelling that can carry `Path::closed`. |
| `shape.stroke` | `color` (**required**), `width` (optional, non-negative) | |
| `shape.fill` | `color` (**required**) | |
| `shape.lifetime` | `"persistent"` \| `{ "ttl_ms": N }` \| `{ "fade_ms": N }` | Exactly one of `ttl_ms` and `fade_ms`. |
| `shape.band` / `shape.z` | `"annotation"` \| `"trail"`; a signed integer | |
| `shape.animation` | `scale` / `opacity` / `translate` / `reveal`, each `{ easing?, duration_ms, … }` | `duration_ms` is **required** on every channel; `easing`, `axis` and `from_edge` default but reject unknown names. |

Colours take `#rrggbb` and `#rrggbbaa`, case-insensitive on input, always
written back lowercase and in the short form when the alpha is opaque.

**The gaps table that used to close this file is empty.** It listed five
spellings the interpreter accepted that no corpus file exercised — error
policy, capture by locator, drag/move/follow option overrides, non-default
buttons, and `ns` waits. `remaining-payload-spellings.json` covers all five and
`overlay-annotated-drag.json` covers options a second time in situ.

Adding a file to either directory registers a test with no CMake edit, so
filling a row costs one file and one `cmake` re-run.
