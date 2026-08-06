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

Data files only. The harness that runs them is a separate unit
(`tests/corpus/run_sequence_corpus.sh`, label `sequence_corpus`).

---

## Contents at a glance

| Directory | Files | Expected verdict |
|---|---|---|
| `valid/` | 7 | `[VALID]` — every one must load |
| `invalid/` | 16 | `[REJECTED]` — every one must be refused |

---

## `valid/` — each of these MUST be accepted

| File | Probes | Notes |
|---|---|---|
| `spec-example.json` | The `login-flow` document from design §3, **verbatim**. | The reference document. It is the only file that spells a button as a string (`"button": "left"`), because the spec does; if the interpreter cannot load this file it does not implement the spec. Also the file the Phase 3 headless smoke test plays. |
| `linear-500.json` | 500 steps with **no `after` anywhere** — the implicit depends-on-the-previous-step edge at scale. | Also has **no `pacing` key at all**, so it probes `strict` being the documented default rather than a required field. Every tenth `input.click` carries a label; the rest are unlabelled, proving an unlabelled step still receives a `StepId` and that the implicit edge points at whatever came before, labelled or not. Generated; one step per line. |
| `diamond.json` | Fork/join: `a → b → d` and `a → c → d`. | All four edges are explicit, so nothing depends on the implicit rule. `d` names two predecessors, which is the fan-in case. |
| `all-sequence-ops.json` | One step per **sequence-capable** op. | There are exactly **15**, not 29 or 30: the descriptor table names 30 commands and only 15 have a payload struct in `include/grab/command.hpp`. Verified against the table — the 15 op strings here are exactly the complement of the 15 rejected by `is_sequence_command()`. `press` is paired with `release` and `key_down` with `key_up`, because the caller owns what it presses. |
| `unicode-type.json` | `input.type` payloads across the UTF-8 and JSON-escape space. | 16 steps: ASCII, 2-byte Latin-1 supplement, Greek/Cyrillic, 3-byte CJK, NFC precomposed vs **genuinely NFD decomposed** (`65 cc 81`, not `c3 a9`), four stacked combining marks on one base, 4-byte emoji with a skin-tone modifier, a ZWJ family, regional-indicator flags, Arabic and Hebrew RTL runs, a mixed-direction run, an explicit U+202E/U+202C bidi override, raw ZWSP/ZWJ/BOM, JSON `\t \n \\ \"` escapes, BMP `\uXXXX` escapes, and a **surrogate-pair** escape (`👍🏽`). |
| `extra-grace-under-strict.json` | `"mode": "strict"` **with** `extra_grace_ms` present on every step. | **This must LOAD AND BE IGNORED, never rejected.** Design §4.10 makes the mode the sole authority; rejecting the field would defeat the whole point of running one document under all three pacing modes without editing it. The largest value (65535 ms) is deliberately absurd — if it were honoured under `strict` the run would visibly stall. |
| `chord-ctrl-c.json` | `key_down Control_L` → `key c` → `key_up Control_L`. | The case that was unreachable before `key_down`/`key_up` entered the table: `Keystroke` carries only shift and altgr, so Ctrl+C cannot be expressed through `type`/`key` alone. |

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

---

## The grammar these files assume

The document shape is pinned by design §3. The **payload field names of ops the
spec does not show are inferred** from the payload struct members in
`include/grab/command.hpp` plus the spec's own conventions (`snake_case`, an
`_ms` suffix on durations, a point as a two-element array). Where a name is
inferred rather than quoted from the spec it is marked below. If the interpreter
chose differently, the mismatch surfaces as a `valid/` file being rejected —
which is a corpus/interpreter reconciliation, not a corpus bug.

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
| `op` | one of the 30 descriptor-table names; only the 15 with a payload struct are accepted | spec §5.2 |
| `after` | array of **labels**; omitted means depends-on-the-previous-step | spec §3 |
| `extra_grace_ms` | integer ≥ 0, read only under `precise` | spec §3, §4.10 |

Payloads:

| Op | Fields used here | Source |
|---|---|---|
| `input.move` | `to: [x, y]` | spec §3 |
| `time.wait` | `ms` | spec §3 |
| `input.click` | `button` (string, spec example only) or omitted for the default | spec §3 |
| `screen.capture` | `out` | spec §3 |
| `input.type` | `text` | spec §3 |
| `input.warp` | `to: [x, y]` | **inferred** — member `to` |
| `input.click_at` | `at: [x, y]` | **inferred** — member `at` |
| `input.press` / `input.release` | none (default button) | **inferred** |
| `input.scroll` | `dx`, `dy` (notches; +dy down, +dx right) | **inferred** — members `dx`/`dy` |
| `input.drag` | `from: [x, y]`, `to: [x, y]` | **inferred** — members `from`/`to` |
| `input.key` / `input.key_down` / `input.key_up` | `key` | **inferred** — member `key` |
| `input.follow` | `path: [[x, y], ...]` — the curve's control points | **inferred, highest risk** — `FollowCommand::path` is a `geometry::Curve` whose member is `control`, so `{"control": [...]}` is an equally defensible spelling and the spec pins neither |

Deliberately **not** exercised, because the spec pins no spelling and a guess
would make a `valid/` file fail for the wrong reason:

- `on_error` (`"abort"` / `"continue"` / `"goto:<label>"`) and `on_error_target`.
- `screen.capture`'s `locator` alternative to `out`.
- `input.drag` / `input.move` / `input.follow` `DragOptions` overrides
  (`interpolation_steps`, `step_dwell`, `path` kind) — all defaulted here.
- Button names beyond `"left"`. `PointerButton` spells them
  `Primary`/`Middle`/`Secondary`, so whether JSON says `"right"` or
  `"secondary"` or `3` is unpinned; only `spec-example.json` names a button, and
  it does so verbatim from the spec.

These are corpus gaps, not design gaps. Fill them once the interpreter fixes the
spelling.
