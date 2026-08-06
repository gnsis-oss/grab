#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/event.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "kernel/presentation/overlay_draw.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace grab::cli
{

    struct SketchOptions
    {
            overlay::DrawKind  kind{ overlay::DrawKind::Rectangle };
            overlay::DrawStyle style{};
    };

    struct SketchBackend
    {
            std::function<Result<overlay::ShapeId>( overlay::Shape )>       add_shape;
            std::function<Result<void>( overlay::ShapeId, overlay::Shape )> update_shape;
            std::function<Result<void>( overlay::ShapeId )>                 remove_shape;
            std::function<Result<void>( std::span<const overlay::ShapeId> )> begin_edit;
            std::function<Result<void>()>                                    end_edit;

            // Draw mode is modal, and while it is armed the overlay must
            // consume pointer input rather than pass it through. Without this,
            // the press that starts a stroke also reaches the desktop below and
            // GNOME begins its own rubber-band selection alongside grab's --
            // sketch learns about the press from the observation stream, which
            // the server delivers regardless of who owns the pointer.
            //
            // Armed when a draw kind is selected, not when the button goes
            // down: by then the press has already been delivered elsewhere.
            std::function<Result<void>()> capture_pointer;
            std::function<Result<void>()> release_pointer;
    };

    // Input-state core shared by the live command and no-display CLI tests.
    // Mouse motion only updates the pending preview; flush_preview() performs
    // at most one overlay mutation for a whole drained input batch.
    class SketchController
    {
        public:

            SketchController( SketchOptions options,
                              SketchBackend backend );

            [[nodiscard]]
            Result<void>
            consume( const Event& event );

            [[nodiscard]]
            Result<void>
            flush_preview();

            [[nodiscard]]
            Result<void>
            finish();

            [[nodiscard]]
            overlay::DrawKind
            kind() const noexcept;

            [[nodiscard]]
            bool
            editing() const noexcept;

            [[nodiscard]]
            bool
            drawing() const noexcept;

            [[nodiscard]]
            bool
            preview_visible() const noexcept;

            [[nodiscard]]
            std::span<const overlay::ShapeId>
            editable_shapes() const noexcept;

            // True while draw mode is consuming pointer input rather than
            // letting it fall through to whatever is underneath.
            [[nodiscard]]
            bool
            capturing() const noexcept
            {
                return captured_;
            }

        private:

            [[nodiscard]]
            Result<void>
            consume_key( const InputKey& key );

            [[nodiscard]]
            Result<void>
            consume_button_down( const MouseButton& button );

            [[nodiscard]]
            Result<void>
            consume_motion( const MouseMove& motion );

            [[nodiscard]]
            Result<void>
            consume_button_up( const MouseButton& button );

            [[nodiscard]]
            Result<void>
            select_kind( overlay::DrawKind kind );

            [[nodiscard]]
            Result<void>
            toggle_edit();

            [[nodiscard]]
            Result<void>
            cancel_draw();

            // Pointer capture for draw mode. Armed on entering a draw kind,
            // disarmed on Escape, on switching to edit mode, and on teardown.
            [[nodiscard]]
            Result<void>
            arm_capture();

            [[nodiscard]]
            Result<void>
            disarm_capture();

            [[nodiscard]]
            Result<void>
            clear_preview();

            [[nodiscard]]
            Result<void>
            commit( SpacePoint at );

            [[nodiscard]]
            Result<void>
                                            remove_last_shape();

            SketchOptions                   options_;
            SketchBackend                   backend_;
            overlay::DrawInteraction        interaction_;
            std::vector<overlay::ShapeId>   editable_;
            std::optional<std::uint32_t>    pressed_button_;
            std::optional<overlay::ShapeId> preview_id_;
            std::optional<overlay::Shape>   pending_preview_;
            bool                            preview_dirty_{};
            bool                            editing_{};
            bool                            captured_{};
    };

    [[nodiscard]]
    Result<SketchOptions>
    parse_sketch_options( std::span<const std::string_view> args );

    int
    run_sketch_command( std::span<char* const> args );

}    // namespace grab::cli
