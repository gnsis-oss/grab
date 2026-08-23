#pragma once

// The public face of grab's command sequences: load a JSON document into a
// validated Sequence, and play it against an open Session.
//
// The vocabulary (StepId, StepStatus, PacingMode, ...) lives in
// grab/sequence_types.hpp and the schema is documented beside the
// interpreter; this header adds the two entry points and nothing else. The
// document format is the authority — a consumer that needs its own step
// vocabulary should own an outer document and embed grab sequences in it,
// not extend this one.
//
// LOAD DIAGNOSTICS ARE THE CONTRACT. Errors name the offending step and
// field the way the interpreter does internally:
//
//     [<file>: ]<json pointer>: [<step designation>: ]<what went wrong>
//
// e.g.  doc.json: /steps/3/after/0: step 'shot': names no step. These files
// are hand-written or LLM-generated; "invalid sequence" helps neither.
//
// PLAY IS THE CLI'S OWN PATH. `grab play` runs through this same loader,
// seat, runner and pump loop, so the CLI keeps proving the API works and
// there is exactly one implementation to drift or not drift.

#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/trace.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab
{

    class Session;

}    // namespace grab

namespace grab::sequence
{

    namespace detail
    {

        struct Access;

    }    // namespace detail

    // A loaded, validated document. Immutable — every mutable thing about a
    // run lives in the run, so playing a Sequence twice is playing the same
    // document twice. Copies are cheap (shared, immutable state).
    class Sequence final
    {
        public:

            // An empty document: zero steps, default pacing. Loadable objects
            // come from load()/load_file().
            Sequence();
            ~Sequence();

            Sequence( const Sequence& );
            Sequence&
            operator=( const Sequence& );
            Sequence( Sequence&& ) noexcept;
            Sequence&
            operator=( Sequence&& ) noexcept;

            // The document's optional name ("sequence" in the JSON), empty
            // when it declared none.
            [[nodiscard]]
            std::string_view
            name() const noexcept;

            [[nodiscard]]
            std::size_t
            step_count() const noexcept;

            [[nodiscard]]
            PacingOptions
            pacing() const noexcept;

            // The StepId behind an author label, or nullopt when no step
            // carries it. Identity is positional — step i is
            // StepId{ i, firstGeneration } — so an unlabelled step is
            // addressable by index even though it cannot be addressed by
            // name.
            [[nodiscard]]
            std::optional<StepId>
            resolve_label( std::string_view label ) const noexcept;

            // The author label of a step, empty when it has none, and the
            // op name ("input.click") of its command. Both answer empty for
            // a StepId this document does not contain.
            [[nodiscard]]
            std::string_view
            label_of( StepId id ) const noexcept;

            [[nodiscard]]
            std::string_view
            op_of( StepId id ) const noexcept;

        private:

            friend struct detail::Access;

            struct Impl;

            explicit Sequence( std::shared_ptr<const Impl> impl ) noexcept;

            std::shared_ptr<const Impl> impl_;
    };

    // Parse a JSON document into a validated Sequence. The error message
    // carries the loader's JSON pointer and step designation — that
    // diagnostic quality is most of the value here.
    [[nodiscard]]
    Result<Sequence>
    load( std::string_view json );

    // The same, from a file; errors are prefixed with the path.
    [[nodiscard]]
    Result<Sequence>
    load_file( const std::filesystem::path& path );

    // The loader backwards. load -> to_json -> load yields an identical
    // document, ids included, because ids are positional and never written
    // into the file.
    [[nodiscard]]
    Result<std::string>
    to_json( const Sequence& sequence );

    struct PlayOptions
    {
            // Pacing overrides, applied over the document's own block the way
            // `grab play --pacing`/`--grace-ms` apply theirs. Absent means
            // the document's value; the two override independently.
            std::optional<PacingMode>                mode{};
            std::optional<std::chrono::milliseconds> grace{};

            // The kill-switch. Polled once per pump; once it reports stop the
            // run is UNWOUND, not abandoned — every entered step is exited in
            // reverse order and every held button, key and pointer capture is
            // released before play() returns.
            std::stop_token stop{};    // NOLINT(readability-redundant-member-init)
    };

    // What one step did. `id` is the positional identity the document
    // assigns, `label` the author's optional handle, `op` the command name.
    struct StepOutcome
    {
            StepId                                  id{};
            std::string                             label{};
            std::string_view                        op{};
            StepStatus                              status{ StepStatus::Pending };

            // The step body's measured call cost, what it declared (nullopt
            // means it declared nothing), and how far past its declaration it
            // ran — the diagnostic that the automated app is slow or hung.
            std::chrono::nanoseconds                call_duration{};
            std::optional<std::chrono::nanoseconds> declared{};
            std::chrono::nanoseconds                overrun{};
    };

    // How a run ended. A step failing under its error policy is reported
    // HERE, not as a Result error: the caller needs to know which step
    // failed, not merely that one did.
    struct PlayReport
    {
            // Done when the frontier emptied; Interrupted when a failure or
            // the stop token unwound the run.
            PlayState                state{ PlayState::Idle };

            // One entry per step, in document order, so steps[i] is the
            // outcome of StepId{ i, firstGeneration }.
            std::vector<StepOutcome> steps{};

            // The failure that ended the run, when one did: the aborting
            // step's error, or Cancelled when the stop token fired.
            std::optional<Error>     failure{};

            // What the unwind released — NotAttempted for a clean run,
            // Released/Failed when something was still down and the unwind
            // lifted it or could not.
            NeutralizationOutcome    neutralization{
                NeutralizationOutcome::NotAttempted,
            };

            // Wall span of the run: last completion minus first entry.
            std::chrono::nanoseconds elapsed{};

            // One per run, so reports from different sessions never collide.
            Uuid                     run_id{};

            [[nodiscard]]
            bool
            succeeded() const noexcept
            {
                return state == PlayState::Done && !failure.has_value();
            }
    };

    // Play a loaded sequence against an open session.
    //
    // Input is injected on the DISPLAY THE SESSION WAS OPENED ON, and overlay
    // steps draw on the session's own surface. The session must stay open for
    // the duration of the call; play() runs on the caller's thread and
    // returns when the run reaches a terminal state.
    //
    // Fails only when the run could not be attempted at all — the session is
    // closed, or its display cannot be reached for input. Everything that
    // happens after the first step enters is reported in the PlayReport,
    // including the failure that aborted a run.
    [[nodiscard]]
    Result<PlayReport>
    play( Session&        session,
          const Sequence& sequence,
          PlayOptions     options = {} );

}    // namespace grab::sequence
