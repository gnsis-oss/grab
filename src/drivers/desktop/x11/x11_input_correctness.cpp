#include "drivers/desktop/x11/x11_input_correctness.hpp"

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::drivers::desktop::x11
{
    namespace
    {

        template<typename Value>
        [[nodiscard]]
        grab::Result<Value>
        failure( grab::ErrorCode code,
                 std::string     message )
        {
            return std::unexpected{
                grab::Error{
                            .code       = code,
                            .message    = std::move( message ),
                            .capability = {},
                            .target     = {},
                            .attempts   = {},
                            }
            };
        }

        template<typename ResultType>
        [[nodiscard]]
        grab::Result<void>
        forward_error( ResultType&& result )
        {
            return std::unexpected{ std::forward<ResultType>( result ).error() };
        }

        class XcbScratchKeycodeBackend final : public ScratchKeycodeBackend
        {
            public:

                explicit XcbScratchKeycodeBackend(
                    xcb_connection_t* connection
                ) noexcept :
                    connection_( connection )
                {
                }

                [[nodiscard]]
                grab::Result<std::vector<ScratchKeycodeMapping>>
                mappings() final
                {
                    if( connection_ == nullptr )
                    {
                        return failure<std::vector<ScratchKeycodeMapping>>(
                            grab::ErrorCode::DeviceInaccessible,
                            "X11 scratch-keycode connection is unavailable"
                        );
                    }

                    const auto* setup = xcb_get_setup( connection_ );
                    if( setup == nullptr || setup->max_keycode < setup->min_keycode )
                    {
                        return failure<std::vector<ScratchKeycodeMapping>>(
                            grab::ErrorCode::ProtocolError,
                            "X11 keycode range is unavailable"
                        );
                    }

                    const auto count = static_cast<std::uint8_t>(
                        setup->max_keycode - setup->min_keycode + 1U
                    );
                    xcb_generic_error_t* error = nullptr;
                    auto*                reply = xcb_get_keyboard_mapping_reply(
                        connection_,
                        xcb_get_keyboard_mapping( connection_,
                                                  setup->min_keycode,
                                                  count ),
                        &error
                    );
                    if( error !=
                        nullptr ||
                        reply ==
                        nullptr ||
                        reply->keysyms_per_keycode == 0U )
                    {
                        std::free( error );
                        std::free( reply );
                        return failure<std::vector<ScratchKeycodeMapping>>(
                            grab::ErrorCode::ProtocolError,
                            "Could not read the X11 keyboard mapping"
                        );
                    }

                    const auto width =
                        static_cast<std::size_t>( reply->keysyms_per_keycode );
                    const auto* symbols = xcb_get_keyboard_mapping_keysyms( reply );
                    std::vector<ScratchKeycodeMapping> result;
                    result.reserve( count );
                    for( std::size_t index = 0U; index < count; ++index )
                    {
                        const auto begin = symbols + ( index * width );
                        result.push_back( ScratchKeycodeMapping{
                            .keycode =
                                static_cast<std::uint8_t>( setup->min_keycode + index ),
                            .keysyms = { begin, begin + width },
                        } );
                    }
                    std::free( reply );
                    return result;
                }

                [[nodiscard]]
                grab::Result<void>
                replace( std::uint8_t                   keycode,
                         std::span<const std::uint32_t> keysyms ) final
                {
                    if( connection_ ==
                        nullptr ||
                        keycode ==
                        0U ||
                        keysyms.empty() ||
                        keysyms.size() > std::numeric_limits<std::uint8_t>::max() )
                    {
                        return failure<void>( grab::ErrorCode::InvalidArgument,
                                              "Invalid X11 scratch-keycode mapping" );
                    }

                    const auto cookie = xcb_change_keyboard_mapping_checked(
                        connection_,
                        1U,
                        keycode,
                        static_cast<std::uint8_t>( keysyms.size() ),
                        keysyms.data()
                    );
                    auto* error = xcb_request_check( connection_, cookie );
                    if( error != nullptr )
                    {
                        std::free( error );
                        return failure<void>(
                            grab::ErrorCode::ProtocolError,
                            "Could not change the X11 scratch-keycode mapping"
                        );
                    }
                    return {};
                }

                [[nodiscard]]
                grab::Result<void>
                fence() final
                {
                    if( connection_ == nullptr )
                    {
                        return failure<void>(
                            grab::ErrorCode::DeviceInaccessible,
                            "X11 scratch-keycode connection is unavailable"
                        );
                    }

                    xcb_generic_error_t* error = nullptr;
                    auto*                reply =
                        xcb_get_input_focus_reply( connection_,
                                                   xcb_get_input_focus( connection_ ),
                                                   &error );
                    std::free( reply );
                    if( error != nullptr )
                    {
                        std::free( error );
                        return failure<void>(
                            grab::ErrorCode::ProtocolError,
                            "X11 scratch-keycode synchronization failed"
                        );
                    }
                    if( reply == nullptr )
                    {
                        return failure<void>(
                            grab::ErrorCode::ProtocolError,
                            "X11 scratch-keycode synchronization failed"
                        );
                    }
                    return {};
                }

            private:

                xcb_connection_t* connection_{};
        };

    }    // namespace

    ModifierGuard::ModifierGuard( ModifierState&                state,
                                  std::span<const std::uint8_t> modifier_keycodes ) :
        state_( &state )
    {
        std::array<bool, 256U> snapshot{};
        for( const auto keycode : modifier_keycodes )
        {
            snapshot[keycode] = keycode != 0U && state_->held( keycode );
        }
        for( std::size_t index = 1U; index < snapshot.size(); ++index )
        {
            if( snapshot[index] )
            {
                const auto keycode = static_cast<std::uint8_t>( index );
                if( state_->set( keycode, false ) )
                {
                    restore_[index] = true;
                }
                else
                {
                    release_succeeded_ = false;
                }
            }
        }
    }

    ModifierGuard::~ModifierGuard()
    {
        static_cast<void>( restore() );
    }

    bool
    ModifierGuard::release_succeeded() const noexcept
    {
        return release_succeeded_;
    }

    bool
    ModifierGuard::restore()
    {
        if( !active_ )
        {
            return true;
        }
        active_        = false;

        bool succeeded = true;
        for( std::size_t index = 1U; index < restore_.size(); ++index )
        {
            if( restore_[index] &&
                !state_->set( static_cast<std::uint8_t>( index ), true ) )
            {
                succeeded = false;
            }
        }
        return succeeded;
    }

    SeatLane::Token
    SeatLane::acquire()
    {
        return Token{ mutex_ };
    }

    std::optional<std::uint8_t>
    find_unused_keycode( std::span<const ScratchKeycodeMapping> mappings )
    {
        for( const auto& mapping : mappings )
        {
            if( mapping.keycode !=
                0U &&
                !mapping.keysyms.empty() &&
                std::ranges::all_of( mapping.keysyms,
                                     []( const std::uint32_t keysym )
                                     {
                                         return keysym == 0U;
                                     } ) )
            {
                return mapping.keycode;
            }
        }
        return std::nullopt;
    }

    ScratchKeycodePool::Loan::Loan( ScratchKeycodePool& pool,
                                    std::uint32_t       keysym,
                                    std::uint8_t        keycode ) noexcept :
        pool_( &pool ),
        keysym_( keysym ),
        keycode_( keycode )
    {
    }

    ScratchKeycodePool::Loan::~Loan()
    {
        if( pool_ != nullptr )
        {
            static_cast<void>( pool_->release( keysym_, keycode_ ) );
        }
    }

    ScratchKeycodePool::Loan::Loan( Loan&& other ) noexcept :
        pool_( std::exchange( other.pool_,
                              nullptr ) ),
        keysym_( std::exchange( other.keysym_,
                                0U ) ),
        keycode_( std::exchange( other.keycode_,
                                 0U ) )
    {
    }

    ScratchKeycodePool::Loan&
    ScratchKeycodePool::Loan::operator=( Loan&& other ) noexcept
    {
        if( this != &other )
        {
            if( pool_ != nullptr )
            {
                static_cast<void>( pool_->release( keysym_, keycode_ ) );
            }
            pool_    = std::exchange( other.pool_, nullptr );
            keysym_  = std::exchange( other.keysym_, 0U );
            keycode_ = std::exchange( other.keycode_, 0U );
        }
        return *this;
    }

    std::uint8_t
    ScratchKeycodePool::Loan::keycode() const noexcept
    {
        return keycode_;
    }

    grab::Result<void>
    ScratchKeycodePool::Loan::restore()
    {
        if( pool_ == nullptr )
        {
            return {};
        }
        auto result = pool_->release( keysym_, keycode_ );
        if( result )
        {
            pool_ = nullptr;
        }
        return result;
    }

    ScratchKeycodePool::ScratchKeycodePool( xcb_connection_t* connection ) :
        backend_( std::make_unique<XcbScratchKeycodeBackend>( connection ) )
    {
    }

    ScratchKeycodePool::ScratchKeycodePool(
        std::unique_ptr<ScratchKeycodeBackend> backend
    ) noexcept :
        backend_( std::move( backend ) )
    {
    }

    ScratchKeycodePool::~ScratchKeycodePool()
    {
        for( const auto& entry : entries_ )
        {
            static_cast<void>( backend_->replace( entry.keycode, entry.original ) );
        }
        if( !entries_.empty() )
        {
            static_cast<void>( backend_->fence() );
        }
    }

    grab::Result<ScratchKeycodePool::Loan>
    ScratchKeycodePool::loan( std::uint32_t keysym )
    {
        const std::scoped_lock lock( mutex_ );
        const auto existing = std::ranges::find( entries_, keysym, &Entry::keysym );
        if( existing != entries_.end() )
        {
            ++existing->references;
            return Loan{ *this, keysym, existing->keycode };
        }

        auto mappings = backend_->mappings();
        if( !mappings )
        {
            return std::unexpected{ std::move( mappings.error() ) };
        }

        const auto selected = std::ranges::find_if(
            *mappings,
            [this]( const ScratchKeycodeMapping& mapping )
            {
                const auto active =
                    std::ranges::find( entries_, mapping.keycode, &Entry::keycode );
                return active ==
                       entries_.end() &&
                       mapping.keycode !=
                       0U &&
                       !mapping.keysyms.empty() &&
                       std::ranges::all_of( mapping.keysyms,
                                            []( const std::uint32_t symbol )
                                            {
                                                return symbol == 0U;
                                            } );
            }
        );
        if( selected == mappings->end() )
        {
            return failure<Loan>( grab::ErrorCode::CapabilityUnavailable,
                                  "No unused X11 scratch keycode is available" );
        }

        auto replacement    = selected->keysyms;
        replacement.front() = keysym;
        auto replace_result = backend_->replace( selected->keycode, replacement );
        if( !replace_result )
        {
            return std::unexpected{ std::move( replace_result.error() ) };
        }
        auto fence_result = backend_->fence();
        if( !fence_result )
        {
            static_cast<void>( backend_->replace( selected->keycode,
                                                  selected->keysyms ) );
            static_cast<void>( backend_->fence() );
            return std::unexpected{ std::move( fence_result.error() ) };
        }

        entries_.push_back( Entry{
            .keysym     = keysym,
            .keycode    = selected->keycode,
            .references = 1U,
            .original   = std::move( selected->keysyms ),
        } );
        return Loan{ *this, keysym, entries_.back().keycode };
    }

    grab::Result<void>
    ScratchKeycodePool::release( std::uint32_t keysym,
                                 std::uint8_t  keycode )
    {
        const std::scoped_lock lock( mutex_ );
        const auto             entry =
            std::ranges::find_if( entries_,
                                  [keysym, keycode]( const Entry& candidate )
                                  {
                                      return candidate.keysym ==
                                             keysym &&
                                             candidate.keycode == keycode;
                                  } );
        if( entry == entries_.end() )
        {
            return {};
        }
        if( entry->references > 1U )
        {
            --entry->references;
            return {};
        }

        auto replace_result = backend_->replace( entry->keycode, entry->original );
        if( !replace_result )
        {
            return forward_error( replace_result );
        }
        auto fence_result = backend_->fence();
        if( !fence_result )
        {
            return forward_error( fence_result );
        }
        entries_.erase( entry );
        return {};
    }

}    // namespace grab::drivers::desktop::x11
