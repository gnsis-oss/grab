#!/usr/bin/env bash
#
# Sequence-corpus harness.
#
#   run_sequence_corpus.sh <grab-binary> <document.json> expect-valid|expect-rejected
#
# Prints diagnostics, then exactly one verdict token as the last line:
#
#   [VALID]     the document loaded and validated cleanly
#   [REJECTED]  the document was refused cleanly, with a pointer-bearing message
#
# ctest matches the token with PASS_REGULAR_EXPRESSION and therefore ignores
# this script's exit status; the token IS the verdict. Anything that is neither
# a clean acceptance nor a clean rejection -- a signal death, a timeout, a
# sanitizer report, a command-line error, a refusal with no diagnostic -- prints
# NO token at all, so the test fails.
#
# Per workspace/CLAUDE.md 6.1 the assertion is NO CRASH AND NO SANITIZER TRIP,
# not "no rejection". A file under invalid/ is *expected* to be refused; a
# crash on one still fails, because an adversarial corpus earns its place by
# pushing the parser, not by proving the parser gives up.
#
# INVARIANT FOR ANY FUTURE EDIT: never write either verdict token into a
# diagnostic line. The invalid/ tests pass on seeing [REJECTED] anywhere in the
# output, so a diagnostic that quoted the token would turn a genuine failure
# into a green test. Emit tokens only through print_verdict.

set -uo pipefail

# ── Vocabulary ───────────────────────────────────────────────

# The only two strings this script is allowed to print in brackets. Keep every
# other message free of them -- see the INVARIANT note in the header.
readonly verdictValid='[VALID]'
readonly verdictRejected='[REJECTED]'

readonly runTimeoutSeconds=60
readonly killGraceSeconds=5
readonly captureLines=40

# grab play exit codes, from unit 11's contract.
readonly grabLoaded=0
readonly grabRefused=1
readonly grabUsageError=2

# timeout(1) reports 124 for its own deadline; a shell reports 128+N for a
# child killed by signal N.
readonly exitTimeout=124
readonly exitSignalBase=128

# jq 1.7 exit codes for `jq -e .`: 0 a value was produced, 1 the parse
# succeeded but the value was null or false, 4 no output at all (empty input),
# 5 a parse error. jq 1.6 reported parse errors as 2, so anything outside
# {0,1,4} is treated as malformed rather than enumerated.
readonly jqProducedValue=0
readonly jqFalseOrNull=1
readonly jqNoOutput=4

# ASan/TSan/MSan headline lines, UBSan's inline report, and every sanitizer's
# SUMMARY. Needed as a separate signal because ASan's default exitcode is 1,
# which collides exactly with grab's "document refused".
readonly sanitizerPattern='AddressSanitizer|LeakSanitizer|ThreadSanitizer|MemorySanitizer|UndefinedBehaviorSanitizer|runtime error:|SUMMARY: .*Sanitizer'

# The refusal message must carry a JSON pointer. Requiring the pointer to be
# followed by ": " is what keeps this from matching the leading "/" of an
# absolute document path -- and ctest passes an absolute path.
readonly pointerPattern='grab: error: .*: /[^:]*: '

# ── Reporting ────────────────────────────────────────────────

# Everything goes to stdout so the ordering ctest shows is the ordering here.

lastCapture=''

dump_capture()
{
    local label=$1
    printf -- '--- %s stdout (first %d lines) ---\n' "$label" "$captureLines"
    head -n "$captureLines" "$scratch/$label.out" 2>/dev/null
    printf -- '--- %s stderr (first %d lines) ---\n' "$label" "$captureLines"
    head -n "$captureLines" "$scratch/$label.err" 2>/dev/null
    printf -- '--- end %s ---\n' "$label"
}

fail()
{
    printf 'FAILURE: %s\n' "$*"
    if [[ -n $lastCapture ]]; then
        dump_capture "$lastCapture"
    fi
    printf 'no verdict token emitted, so this test fails.\n'
    exit 1
}

print_verdict()
{
    printf '%s\n' "$1"
}

# ── Arguments ────────────────────────────────────────────────

if [[ $# -ne 3 ]]; then
    printf 'FAILURE: expected 3 arguments, got %d\n' "$#"
    printf 'usage: run_sequence_corpus.sh <grab-binary> <document.json> expect-valid|expect-rejected\n'
    exit 1
fi

grabBinary=$1
document=$2
expectation=$3

case $expectation in
    expect-valid | expect-rejected ) ;;
    * )
        printf "FAILURE: unknown expectation '%s'; want expect-valid or expect-rejected\n" \
               "$expectation"
        exit 1
        ;;
esac

if [[ ! -x $grabBinary ]]; then
    printf "FAILURE: grab binary '%s' is missing or not executable\n" "$grabBinary"
    exit 1
fi

# -f, not -s: a zero-length document is a corpus case, not a missing file.
if [[ ! -f $document || ! -r $document ]]; then
    printf "FAILURE: corpus document '%s' is missing or unreadable\n" "$document"
    exit 1
fi

if ! scratch=$( mktemp -d ); then
    printf 'FAILURE: could not create a scratch directory\n'
    exit 1
fi
readonly scratch
trap 'rm -rf "$scratch"' EXIT

printf 'document:    %s\n' "$document"
printf 'expectation: %s\n' "$expectation"

# ── Canonical-tool comparison: jq well-formedness ────────────
#
# `jq empty` is the WRONG oracle here: it reads zero inputs from a zero-length
# file and so exits 0, declaring the one file that exists to catch an empty
# input to be fine. Use `jq -e .`, and check for a zero-length file first
# anyway so the verdict does not depend on which jq is installed.

jqVerdict=unavailable
jqDetail='jq is not on PATH'

if command -v jq > /dev/null 2>&1; then
    if [[ ! -s $document ]]; then
        jqVerdict=malformed
        jqDetail='zero-length file (jq empty would exit 0 here; jq -e . exits 4)'
    else
        jq -e . "$document" > /dev/null 2> "$scratch/jq.err"
        jqStatus=$?
        case $jqStatus in
            "$jqProducedValue" | "$jqFalseOrNull" )
                jqVerdict=well-formed
                jqDetail="jq -e . exited $jqStatus"
                ;;
            "$jqNoOutput" )
                jqVerdict=malformed
                jqDetail='jq -e . produced no output (exit 4)'
                ;;
            * )
                jqVerdict=malformed
                jqDetail="jq -e . exited $jqStatus: $( head -n 1 "$scratch/jq.err" )"
                ;;
        esac
    fi
fi

printf 'jq:          %s (%s)\n' "$jqVerdict" "$jqDetail"

# ── Running grab ─────────────────────────────────────────────
#
# Every invocation goes through here so that the crash, timeout and sanitizer
# checks apply to the pacing sweep as well as to the primary run.

run_grab()
{
    local label=$1
    shift

    lastCapture=$label

    timeout --kill-after="${killGraceSeconds}s" "${runTimeoutSeconds}s" \
            "$grabBinary" play "$document" --dry-run "$@" \
            > "$scratch/$label.out" 2> "$scratch/$label.err"
    local status=$?

    # Sanitizer first: an ASan report aborts, and attributing that to SIGABRT
    # rather than to the sanitizer would bury the only useful line.
    if grep -Eq "$sanitizerPattern" "$scratch/$label.out" "$scratch/$label.err"; then
        fail "a sanitizer reported on this document (run '$label')"
    fi

    if [[ $status -eq $exitTimeout ]]; then
        fail "grab did not finish within ${runTimeoutSeconds}s (run '$label')"
    fi

    if [[ $status -ge $exitSignalBase ]]; then
        fail "grab died on signal $(( status - exitSignalBase )) (run '$label'); a crash fails the test whichever directory the document sits in"
    fi

    if [[ $status -eq $grabUsageError ]]; then
        fail "grab exited $grabUsageError, a command-line error (run '$label'); that is a harness bug, not a verdict on the document"
    fi

    return $status
}

run_grab base
grabStatus=$?

printf 'grab exit:   %d\n' "$grabStatus"

case $grabStatus in
    "$grabLoaded" )  observed=valid ;;
    "$grabRefused" ) observed=rejected ;;
    * )
        fail "grab exited $grabStatus, which is neither $grabLoaded (loaded) nor $grabRefused (refused)"
        ;;
esac

printf 'grab:        %s\n' "$observed"

# ── The two contracts, checked rather than assumed ───────────

if [[ $observed == valid ]]; then
    for line in '^sequence: ' '^steps: [0-9]+$' '^order: ' '^plan: '; do
        if ! grep -Eq "$line" "$scratch/base.out"; then
            fail "grab exited 0 but its --dry-run plan has no line matching /$line/; the plan contract has changed"
        fi
    done
else
    # An empty stderr on failure is indistinguishable from a silent death, so
    # it is treated as one.
    if [[ ! -s $scratch/base.err ]]; then
        fail 'grab refused the document with an empty stderr; a refusal carrying no diagnostic is indistinguishable from a silent death'
    fi
    if ! grep -q 'grab: error:' "$scratch/base.err"; then
        fail 'grab refused the document but stderr carries no "grab: error:" line'
    fi
    if ! grep -Eq "$pointerPattern" "$scratch/base.err"; then
        fail 'the refusal message carries no JSON pointer; design 5.2 requires the loader to name the offending location'
    fi
fi

# ── Oracle comparison ────────────────────────────────────────
#
# The only direction that is a bug is jq-rejects / grab-accepts: grab would
# have accepted bytes that are not JSON. The opposite direction is routine and
# expected -- jq answers "is this JSON", grab answers "is this a sequence
# document", and every schema, root-type and graph rule lives only in the
# second question.

if [[ $jqVerdict == unavailable ]]; then
    printf 'oracle:      UNAVAILABLE -- %s; the canonical-tool comparison did not run\n' \
           "$jqDetail"
elif [[ $jqVerdict == malformed && $observed == valid ]]; then
    fail 'jq cannot parse this document but grab accepted it; grab accepted bytes that are not JSON'
elif [[ $jqVerdict == well-formed && $observed == valid ]]; then
    printf 'oracle:      agree -- both accept\n'
elif [[ $jqVerdict == malformed && $observed == rejected ]]; then
    printf 'oracle:      agree -- both reject, and the fault is in the JSON grammar itself\n'
else
    printf 'oracle:      higher-level rejection -- jq parses it, grab refuses it on a document rule (root type, schema or graph). Expected for a syntactically fine invalid/ file.\n'
fi

# ── Pacing invariant, for accepted documents only ────────────
#
# Design 4.10 makes the pacing mode the sole authority over *timing* and
# nothing else: the same document under strict, grace and precise must yield
# the same steps in the same order, and only the planned total may move. This
# is the one assertion the corpus can make that no unit test makes over real
# files, and it is what proves extra-grace-under-strict.json is ignored rather
# than rejected.

if [[ $observed == valid ]]; then
    baselineOrder=$( grep -m 1 '^order: ' "$scratch/base.out" )
    strictPlan=''

    for mode in strict grace precise; do
        run_grab "$mode" --pacing "$mode"
        modeStatus=$?

        if [[ $modeStatus -ne $grabLoaded ]]; then
            fail "the document loads under its own pacing but exits $modeStatus under --pacing $mode"
        fi

        modeOrder=$( grep -m 1 '^order: ' "$scratch/$mode.out" )
        if [[ $modeOrder != "$baselineOrder" ]]; then
            fail "--pacing $mode changed the step order; the mode is supposed to govern timing only"
        fi

        modePlan=$( grep -m 1 '^plan: ' "$scratch/$mode.out" \
                    | sed -n 's/^plan: >= \([0-9]\{1,\}\) ms.*/\1/p' )
        if [[ -z $modePlan ]]; then
            fail "--pacing $mode printed no parseable 'plan: >= N ms' line"
        fi

        if [[ $mode == strict ]]; then
            strictPlan=$modePlan
        elif [[ $modePlan -lt $strictPlan ]]; then
            fail "--pacing $mode plans ${modePlan} ms, less than strict's ${strictPlan} ms; neither grace nor precise may remove time"
        fi

        printf 'pacing:      %-7s order stable, plan >= %s ms\n' "$mode" "$modePlan"
    done

    lastCapture=base
fi

# ── Verdict ──────────────────────────────────────────────────

if [[ $expectation == expect-valid && $observed == valid ]]; then
    print_verdict "$verdictValid"
    exit 0
fi

if [[ $expectation == expect-rejected && $observed == rejected ]]; then
    print_verdict "$verdictRejected"
    exit 0
fi

fail "expected the document to be ${expectation#expect-}, but grab reported ${observed}"
