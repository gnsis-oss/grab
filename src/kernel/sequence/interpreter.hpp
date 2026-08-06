#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// JSON bytes in, a typed Sequence out.
//
//   JSON bytes
//     -> nlohmann::json
//     -> op string  -> grab::command_kind()   unknown -> error naming the op
//     -> payload    -> one parser per CommandKind
//     -> ids + after -> Sequence::build()     which owns graph and cycle checks
//     -> Result<Sequence>
//
// The loader rejects what it cannot BUILD — unknown op, an op with no payload
// struct, a dangling `after`, a duplicate label, a self-edge, a cycle. Those
// are not policy: a cycle does not fail validation, it fails to terminate.
// Policy lives in validate(), which is a declared pass-through seam.
//
// Errors carry the step's label AND a JSON pointer (/steps/3/after/0). These
// files are hand-written or LLM-generated; "invalid sequence" helps neither.
// Every message reads
//
//     [<file>: ]<json pointer>: [<step designation>: ]<what went wrong>
//
// where the designation is `step 'label'` when the author gave one and
// `step at index N` when they did not — a document may be entirely unlabelled,
// so a label alone cannot locate a fault.
//
// ── THE GRAMMAR ──────────────────────────────────────────────
//
//   { "schema_version": 1,                  optional, must be 1 when present
//     "sequence": "login-flow",             optional document name
//     "pacing": { "mode": "grace",          strict | grace | precise
//                 "grace_ms": 80 },         optional, defaults to strict/0
//     "steps": [ ... ] }                    required
//
// A step is
//
//   { "id": "click",                        OPTIONAL AUTHOR LABEL, not identity
//     "op": "input.click",                  required, a CommandDescriptor name
//     "after": [ "move", 3 ],               optional, see below
//     "on_error": "abort",                  abort | continue | goto:<label>
//     "extra_grace_ms": 400,                optional, read only under `precise`
//     ...payload fields per op... }
//
// `id` is an author handle. Identity is the StepId, assigned POSITIONALLY by
// Sequence::build whether or not a label exists, so two byte-identical
// unlabelled `input.click` steps are still different steps. Identity is
// therefore never written into the document, which is exactly what lets
// parse -> to_json -> parse come back identical.
//
// **A step with no `after` depends on the step before it in document order**,
// and the first step depends on nothing. That is what makes a plain list read
// top-to-bottom like the bash script it replaces. `"after": []` is NOT the same
// as an absent `after`: it declares a root explicitly.
//
// An `after` entry is a label (string) or a document index (integer). The index
// form exists because a step with no label must still be nameable as a
// predecessor — to_json emits it when serializing a graph whose predecessor
// carries no label.
//
// `extra_grace_ms` LOADS UNDER EVERY MODE and is ignored outside `precise`.
// Rejecting it under `strict` would defeat running one document under all three
// modes, which is the whole point of having modes: the mode is the sole
// authority, not the step.
//
// Unknown object keys are ignored rather than rejected. Every required field is
// checked by name, so a typo in one is still caught where it matters.
//
// ── PAYLOADS ─────────────────────────────────────────────────
//
//   input.type       "text": "hi"
//   input.key        "key": "Return"
//   input.key_down   "key": "Control_L"
//   input.key_up     "key": "Control_L"
//   input.click      "button": "left" | 1        (optional, defaults to left)
//   input.click_at   "at": [x, y], "button"
//   input.press      "button"
//   input.release    "button"
//   input.scroll     "dx": 0, "dy": 3            (notches; +dy down, +dx right)
//   input.warp       "to": [x, y]
//   input.move       "to": [x, y], "from": [x, y] (optional), "options"
//   input.follow     "curve": [[x, y], ...], "options"
//   input.drag       "from": [x, y], "to": [x, y], "button", "options"
//   screen.capture   exactly one of "out": "a.png" / "locator": "..."
//   time.wait        exactly one of "ms": 250 / "ns": 250000000
//
// `options` is the DragOptions triple, omitted when it is the default:
//   { "steps": 16, "step_dwell_ms": 8, "path": "linear" | "cubic" }
//
// The 15 payload structs above are the whole sequence surface. The descriptor
// table names 30 commands; `system.doctor`, `service.daemon`, `screen.watch`,
// `session.open`, `screen.batch`, `image.compare`, `input.drag_curve`,
// `screen.windows`, `window.focus`, `window.place`, `system.play` and the four
// `overlay.*` kinds resolve through command_kind() but have no payload here and
// are rejected as "is not available as a sequence step" — a DIFFERENT message
// from "unknown op", because they are different author mistakes.

#include "grab/result.hpp"
#include "kernel/sequence/sequence.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace grab::kernel::sequence
{

    [[nodiscard]]
    grab::Result<Sequence>
    parse( std::string_view json );

    [[nodiscard]]
    grab::Result<Sequence>
    load( const std::filesystem::path& path );

    // The loader backwards. load -> to_json -> load must yield an identical
    // Sequence, ids included — which works only because ids are positional and
    // therefore never written into the document.
    //
    // It fails only where a Sequence holds something the grammar cannot spell:
    // a CaptureCommand with neither or both of output and locator, or an
    // `after` entry naming a step the document does not contain. Neither is
    // reachable through parse().
    [[nodiscard]]
    grab::Result<std::string>
    to_json( const Sequence& sequence );

}    // namespace grab::kernel::sequence
