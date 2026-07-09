#include "core/json.hpp"
#include "event/state.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace grab::event
{
    namespace
    {

        constexpr std::uint64_t    kUnsetSequence = 0U;
        constexpr std::string_view kOpenKey       = "open";
        constexpr std::string_view kFocusedKey    = "focused";
        constexpr std::string_view kAppKey        = "app";
        constexpr std::string_view kPidKey        = "pid";
        constexpr std::string_view kTitleKey      = "title";

        using detail::WindowRecord;

        [[nodiscard]]
        bool
        is_empty_window( const grab::WindowChange& change ) noexcept
        {
            return change.app.empty() && change.pid.empty() && change.title.empty();
        }

        [[nodiscard]]
        bool
        same_identity( const WindowRecord&       record,
                       const grab::WindowChange& change ) noexcept
        {
            return record.app == change.app && record.title == change.title;
        }

        [[nodiscard]]
        WindowRecord
        make_record( const grab::WindowChange& change )
        {
            return WindowRecord{
                .app   = change.app,
                .pid   = change.pid,
                .title = change.title,
            };
        }

        void
        write_window( grab::core::json::Writer& writer,
                      const WindowRecord&       window )
        {
            writer.field( kAppKey, window.app );
            writer.field( kPidKey, window.pid );
            writer.field( kTitleKey, window.title );
        }

    }    // namespace

    StateManager::StateManager() = default;

    void
    StateManager::observe( const grab::Event& event )
    {
        const auto* change = std::get_if<grab::WindowChange>( &event.payload );
        if( change == nullptr )
        {
            return;
        }

        switch( event.kind )
        {
            case grab::EventKind::window_created :
                {
                    auto existing =
                        std::ranges::find_if( open_windows_,
                                              [change]( const WindowRecord& record )
                                              {
                                                  return same_identity( record,
                                                                        *change );
                                              } );
                    if( existing == open_windows_.end() )
                    {
                        open_windows_.push_back( make_record( *change ) );
                    }
                    else
                    {
                        *existing = make_record( *change );
                    }
                    break;
                }
            case grab::EventKind::window_closed :
                {
                    const auto removed =
                        std::ranges::remove_if( open_windows_,
                                                [change]( const WindowRecord& record )
                                                {
                                                    return same_identity( record,
                                                                          *change );
                                                } );
                    open_windows_.erase( removed.begin(), removed.end() );

                    if( has_focused_window_ && same_identity( focused_, *change ) )
                    {
                        focused_            = WindowRecord{};
                        has_focused_window_ = false;
                    }
                    break;
                }
            case grab::EventKind::window_focus_changed :
                if( is_empty_window( *change ) )
                {
                    focused_            = WindowRecord{};
                    has_focused_window_ = false;
                }
                else
                {
                    focused_            = make_record( *change );
                    has_focused_window_ = true;
                }
                break;
            default :
                break;
        }
    }

    grab::Event
    StateManager::snapshot( double timestamp ) const
    {
        grab::core::json::Writer writer;
        writer.begin_object();
        writer.begin_array( kOpenKey );
        for( const auto& window : open_windows_ )
        {
            writer.begin_object_in_array();
            write_window( writer, window );
            writer.end_object();
        }
        writer.end_array();
        writer.field_object_start( kFocusedKey );
        if( has_focused_window_ )
        {
            write_window( writer, focused_ );
        }
        else
        {
            write_window( writer, WindowRecord{} );
        }
        writer.end_object();
        writer.end_object();

        return grab::Event{
            .timestamp = timestamp,
            .sequence  = kUnsetSequence,
            .kind      = grab::EventKind::state_snapshot,
            .category  = grab::category_of( grab::EventKind::state_snapshot ),
            .payload   = grab::Payload{ grab::StateSnapshot{
                .json = std::move( writer ).take(),
            } },
        };
    }

    void
    StateManager::publish_snapshot( grab::EventBus& bus,
                                    double          timestamp ) const
    {
        bus.publish( snapshot( timestamp ) );
    }

    std::size_t
    StateManager::open_window_count() const noexcept
    {
        return open_windows_.size();
    }

}    // namespace grab::event
