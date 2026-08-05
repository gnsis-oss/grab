#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"
#include "grab/result.hpp"

#include <functional>
#include <memory>
#include <span>

namespace grab
{

    struct EditCallbacks
    {
            std::function<void( overlay::ShapeId, const overlay::Shape& )> on_edit;
            std::function<void( overlay::ShapeId )>                        on_cancelled;
    };

    class EditSession
    {
        public:

            ~EditSession();

            EditSession( const EditSession& ) = delete;
            EditSession&
            operator=( const EditSession& ) = delete;
            EditSession( EditSession&& ) noexcept;
            EditSession&
            operator=( EditSession&& ) noexcept;

            [[nodiscard]]
            Result<void>
            status() const;

        private:

            friend Result<EditSession>
            overlay_edit( Overlay&,
                          std::span<const overlay::ShapeId>,
                          EditCallbacks );

            class Impl;

            explicit EditSession( std::unique_ptr<Impl> impl ) noexcept;

            [[nodiscard]]
            static Result<EditSession>
                                  create( Overlay&                          overlay,
                                          std::span<const overlay::ShapeId> editable,
                                          EditCallbacks                     callbacks );

            std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]]
    Result<EditSession>
    overlay_edit( Overlay&                          overlay,
                  std::span<const overlay::ShapeId> editable,
                  EditCallbacks                     callbacks );

}    // namespace grab
