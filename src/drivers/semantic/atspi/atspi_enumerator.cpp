#include "drivers/semantic/atspi/atspi_dbus.hpp"
#include "drivers/semantic/atspi/atspi_enumerator.hpp"
#include "drivers/semantic/atspi/atspi_tree_source.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "grab/ui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace grab::drivers::semantic::atspi
{
    namespace
    {

        // --- AT-SPI2 addressing -------------------------------------------------

        constexpr const char*   registryDest        = "org.a11y.atspi.Registry";
        constexpr const char*   rootPath            = "/org/a11y/atspi/accessible/root";

        constexpr const char*   accessibleInterface = "org.a11y.atspi.Accessible";
        constexpr const char*   componentInterface  = "org.a11y.atspi.Component";
        constexpr const char*   textInterface       = "org.a11y.atspi.Text";
        constexpr const char*   hyperlinkInterface  = "org.a11y.atspi.Hyperlink";
        constexpr const char*   propertiesInterface = "org.freedesktop.DBus.Properties";

        constexpr const char*   getChildrenMethod   = "GetChildren";
        constexpr const char*   getRoleNameMethod   = "GetRoleName";
        constexpr const char*   getStateMethod      = "GetState";
        constexpr const char*   getInterfacesMethod = "GetInterfaces";
        constexpr const char*   getExtentsMethod    = "GetExtents";
        constexpr const char*   getTextMethod       = "GetText";
        constexpr const char*   getUriMethod        = "GetURI";
        constexpr const char*   propertiesGetMethod = "Get";

        constexpr const char*   nameProperty        = "Name";
        constexpr const char*   characterCountProperty = "CharacterCount";

        // Component.GetExtents coordinate type: 0 = screen, 1 = window. Spider
        // clicks in screen space, so screen extents are the click targets.
        constexpr std::uint32_t coordScreen            = 0U;

        constexpr const char*   actionInterfaceName    = "org.a11y.atspi.Action";
        constexpr const char*   valueInterfaceName     = "org.a11y.atspi.Value";
        constexpr const char*   selectionInterfaceName = "org.a11y.atspi.Selection";

        // AT-SPI StateType bit positions (from the AtspiStateType enum, verified
        // against the live introspection). GetState returns a two-word bitset;
        // bit N lives in word N/32 at offset N%32.
        constexpr int           atspiStateActive   = 1;
        constexpr int           atspiStateEditable = 7;
        constexpr int           atspiStateEnabled  = 8;
        constexpr int           atspiStateExpanded = 10;
        constexpr int           atspiStateFocused  = 12;
        constexpr int           atspiStateShowing  = 25;
        constexpr int           atspiStateSelected = 23;
        constexpr int           atspiStateBusy     = 3;

        constexpr int           stateWordBits      = 32;

        // --- role name -> AtspiRole --------------------------------------------

        // GetRoleName returns the stable, non-localized machine name. This table
        // covers the roles a document tree exposes; anything else maps to Unknown
        // and still carries geometry/text.
        using RoleEntry = std::pair<std::string_view, AtspiRole>;

        [[nodiscard]]
        AtspiRole
        role_from_name( std::string_view name )
        {
            static constexpr std::array<RoleEntry, 35U> table{
                {
                 { "application", AtspiRole::Application },
                 { "frame", AtspiRole::Frame },
                 { "window", AtspiRole::Window },
                 { "document web", AtspiRole::Document },
                 { "document frame", AtspiRole::Document },
                 { "document", AtspiRole::Document },
                 { "dialog", AtspiRole::Dialog },
                 { "alert", AtspiRole::Alert },
                 { "panel", AtspiRole::Panel },
                 { "section", AtspiRole::Section },
                 { "filler", AtspiRole::Section },
                 { "push button", AtspiRole::PushButton },
                 { "button", AtspiRole::Button },
                 { "toggle button", AtspiRole::ToggleButton },
                 { "check box", AtspiRole::CheckBox },
                 { "radio button", AtspiRole::RadioButton },
                 { "entry", AtspiRole::Entry },
                 { "password text", AtspiRole::PasswordText },
                 { "text", AtspiRole::Text },
                 { "paragraph", AtspiRole::Paragraph },
                 { "heading", AtspiRole::Heading },
                 { "link", AtspiRole::Link },
                 { "image", AtspiRole::Image },
                 { "list", AtspiRole::List },
                 { "list item", AtspiRole::ListItem },
                 { "table", AtspiRole::Table },
                 { "table cell", AtspiRole::TableCell },
                 { "menu", AtspiRole::Menu },
                 { "menu bar", AtspiRole::MenuBar },
                 { "menu item", AtspiRole::MenuItem },
                 { "page tab", AtspiRole::PageTab },
                 { "page tab list", AtspiRole::PageTabList },
                 { "slider", AtspiRole::Slider },
                 { "spin button", AtspiRole::SpinButton },
                 { "combo box", AtspiRole::ComboBox },
                 }
            };
            for( const auto& [role_name, role] : table )
            {
                if( role_name == name )
                {
                    return role;
                }
            }
            return AtspiRole::Unknown;
        }

        // --- libdbus reply readers ---------------------------------------------

        [[nodiscard]]
        std::string
        read_string_reply( const dbus::Message& reply )
        {
            DBusMessageIter iterator{};
            if( dbus_message_iter_init( reply.get(), &iterator ) ==
                0 ||
                dbus_message_iter_get_arg_type( &iterator ) != DBUS_TYPE_STRING )
            {
                return {};
            }
            const char* value = nullptr;
            dbus_message_iter_get_basic( &iterator, static_cast<void*>( &value ) );
            return value == nullptr ? std::string{} : std::string{ value };
        }

        // Reads Properties.Get reply (a variant) as a string. Used for Name.
        [[nodiscard]]
        std::string
        read_variant_string( const dbus::Message& reply )
        {
            DBusMessageIter iterator{};
            if( dbus_message_iter_init( reply.get(), &iterator ) ==
                0 ||
                dbus_message_iter_get_arg_type( &iterator ) != DBUS_TYPE_VARIANT )
            {
                return {};
            }
            DBusMessageIter variant{};
            dbus_message_iter_recurse( &iterator, &variant );
            if( dbus_message_iter_get_arg_type( &variant ) != DBUS_TYPE_STRING )
            {
                return {};
            }
            const char* value = nullptr;
            dbus_message_iter_get_basic( &variant, static_cast<void*>( &value ) );
            return value == nullptr ? std::string{} : std::string{ value };
        }

        // Reads Properties.Get reply (a variant) as an int32. Used for
        // CharacterCount.
        [[nodiscard]]
        std::optional<std::int32_t>
        read_variant_int( const dbus::Message& reply )
        {
            DBusMessageIter iterator{};
            if( dbus_message_iter_init( reply.get(), &iterator ) ==
                0 ||
                dbus_message_iter_get_arg_type( &iterator ) != DBUS_TYPE_VARIANT )
            {
                return std::nullopt;
            }
            DBusMessageIter variant{};
            dbus_message_iter_recurse( &iterator, &variant );
            if( dbus_message_iter_get_arg_type( &variant ) != DBUS_TYPE_INT32 )
            {
                return std::nullopt;
            }
            std::int32_t value = 0;
            dbus_message_iter_get_basic( &variant, static_cast<void*>( &value ) );
            return value;
        }

        // Reads a(so): array of (bus-name, object-path) references.
        [[nodiscard]]
        std::vector<std::pair<std::string,
                              std::string>>
        read_object_refs( const dbus::Message& reply )
        {
            std::vector<std::pair<std::string, std::string>> refs;
            DBusMessageIter                                  iterator{};
            if( dbus_message_iter_init( reply.get(), &iterator ) ==
                0 ||
                dbus_message_iter_get_arg_type( &iterator ) != DBUS_TYPE_ARRAY )
            {
                return refs;
            }
            DBusMessageIter array{};
            dbus_message_iter_recurse( &iterator, &array );
            while( dbus_message_iter_get_arg_type( &array ) == DBUS_TYPE_STRUCT )
            {
                DBusMessageIter entry{};
                dbus_message_iter_recurse( &array, &entry );
                const char* bus_name = nullptr;
                const char* path     = nullptr;
                if( dbus_message_iter_get_arg_type( &entry ) == DBUS_TYPE_STRING )
                {
                    dbus_message_iter_get_basic( &entry,
                                                 static_cast<void*>( &bus_name ) );
                    dbus_message_iter_next( &entry );
                    if( dbus_message_iter_get_arg_type( &entry ) ==
                        DBUS_TYPE_OBJECT_PATH )
                    {
                        dbus_message_iter_get_basic( &entry,
                                                     static_cast<void*>( &path ) );
                    }
                }
                if( bus_name != nullptr && path != nullptr )
                {
                    refs.emplace_back( std::string{ bus_name }, std::string{ path } );
                }
                dbus_message_iter_next( &array );
            }
            return refs;
        }

        // Reads (iiii): a bounding rectangle. Empty when the extents are the
        // AT-SPI "not on screen" sentinel of a zero-area or negative rect.
        [[nodiscard]]
        std::optional<grab::SpaceRect>
        read_extents( const dbus::Message& reply )
        {
            DBusMessageIter iterator{};
            if( dbus_message_iter_init( reply.get(), &iterator ) ==
                0 ||
                dbus_message_iter_get_arg_type( &iterator ) != DBUS_TYPE_STRUCT )
            {
                return std::nullopt;
            }
            DBusMessageIter fields{};
            dbus_message_iter_recurse( &iterator, &fields );
            std::array<std::int32_t, 4U> values{ 0, 0, 0, 0 };
            for( auto& slot : values )
            {
                if( dbus_message_iter_get_arg_type( &fields ) != DBUS_TYPE_INT32 )
                {
                    return std::nullopt;
                }
                dbus_message_iter_get_basic( &fields, static_cast<void*>( &slot ) );
                dbus_message_iter_next( &fields );
            }
            if( values[2] <= 0 || values[3] <= 0 )
            {
                return std::nullopt;
            }
            return grab::SpaceRect{
                .x = static_cast<double>( values[0] ),
                .y = static_cast<double>( values[1] ),
                .w = static_cast<double>( values[2] ),
                .h = static_cast<double>( values[3] ),
            };
        }

        // Reads au: the two-word AT-SPI state bitset, mapped to grab NodeState.
        [[nodiscard]]
        std::uint32_t
        read_states( const dbus::Message& reply )
        {
            DBusMessageIter iterator{};
            if( dbus_message_iter_init( reply.get(), &iterator ) ==
                0 ||
                dbus_message_iter_get_arg_type( &iterator ) != DBUS_TYPE_ARRAY )
            {
                return 0U;
            }
            DBusMessageIter array{};
            dbus_message_iter_recurse( &iterator, &array );
            std::array<std::uint32_t, 2U> words{ 0U, 0U };
            std::size_t                   index = 0U;
            while( dbus_message_iter_get_arg_type( &array ) ==
                   DBUS_TYPE_UINT32 &&
                   index < words.size() )
            {
                dbus_message_iter_get_basic( &array,
                                             static_cast<void*>( &words.at( index ) ) );
                dbus_message_iter_next( &array );
                ++index;
            }

            const auto has = [&words]( int bit ) -> bool
            {
                const std::uint32_t word = ( bit < stateWordBits ) ? words[0] : words[1];
                const int shift = ( bit < stateWordBits ) ? bit : bit - stateWordBits;
                return ( ( word >> static_cast<std::uint32_t>( shift ) ) & 1U ) != 0U;
            };

            std::uint32_t states = 0U;
            if( has( atspiStateActive ) )
            {
                states |= grab::NodeState::Active;
            }
            if( has( atspiStateFocused ) )
            {
                states |= grab::NodeState::Focused;
            }
            if( has( atspiStateShowing ) )
            {
                states |= grab::NodeState::Visible;
            }
            if( has( atspiStateSelected ) )
            {
                states |= grab::NodeState::Selected;
            }
            if( has( atspiStateEnabled ) )
            {
                states |= grab::NodeState::Enabled;
            }
            if( has( atspiStateEditable ) )
            {
                states |= grab::NodeState::Editable;
            }
            if( has( atspiStateExpanded ) )
            {
                states |= grab::NodeState::Expanded;
            }
            if( has( atspiStateBusy ) )
            {
                states |= grab::NodeState::Busy;
            }
            return states;
        }

        // Reads as: the interface-name list, into the AtspiInterfaceSet plus the
        // component/text/hyperlink flags the walk needs for conditional reads.
        struct InterfacePresence
        {
                AtspiInterfaceSet set{};
                bool              component{};
                bool              text{};
                bool              hyperlink{};
        };

        [[nodiscard]]
        InterfacePresence
        read_interfaces( const dbus::Message& reply )
        {
            InterfacePresence presence;
            DBusMessageIter   iterator{};
            if( dbus_message_iter_init( reply.get(), &iterator ) ==
                0 ||
                dbus_message_iter_get_arg_type( &iterator ) != DBUS_TYPE_ARRAY )
            {
                return presence;
            }
            DBusMessageIter array{};
            dbus_message_iter_recurse( &iterator, &array );
            while( dbus_message_iter_get_arg_type( &array ) == DBUS_TYPE_STRING )
            {
                const char* name = nullptr;
                dbus_message_iter_get_basic( &array, static_cast<void*>( &name ) );
                const std::string_view view{ name == nullptr ? "" : name };
                if( view == componentInterface )
                {
                    presence.component = true;
                }
                else if( view == textInterface )
                {
                    presence.text     = true;
                    presence.set.text = true;
                }
                else if( view == hyperlinkInterface )
                {
                    presence.hyperlink = true;
                }
                else if( view == actionInterfaceName )
                {
                    presence.set.action = true;
                }
                else if( view == valueInterfaceName )
                {
                    presence.set.value = true;
                }
                else if( view == selectionInterfaceName )
                {
                    presence.set.selection = true;
                }
                dbus_message_iter_next( &array );
            }
            return presence;
        }

        // --- blocking method-call helpers --------------------------------------

        // Sends a method call and blocks for the reply; nullptr reply is an
        // error (caller decides whether that node is fatal or skippable).
        template<typename Appender>
        [[nodiscard]]
        dbus::Message
        call( DBusConnection* connection,
              const char*     destination,
              const char*     path,
              const char*     interface,
              const char*     method,
              const Appender& append )
        {
            dbus::Message request{
                dbus_message_new_method_call( destination, path, interface, method )
            };
            if( request == nullptr )
            {
                return dbus::Message{};
            }
            DBusMessageIter iterator{};
            dbus_message_iter_init_append( request.get(), &iterator );
            append( iterator );

            dbus::Error   reply_error;
            dbus::Message reply{
                dbus_connection_send_with_reply_and_block( connection,
                                                           request.get(),
                                                           dbus::dbusCallTimeoutMs,
                                                           &reply_error.value )
            };
            return reply;
        }

        void
        append_none( DBusMessageIter& /*iterator*/ )
        {
        }

        void
        append_string( DBusMessageIter& iterator,
                       const char*      value )
        {
            static_cast<void>(
                dbus_message_iter_append_basic( &iterator,
                                                DBUS_TYPE_STRING,
                                                static_cast<const void*>( &value ) )
            );
        }

        // --- the walk ----------------------------------------------------------

        struct EnumeratorState
        {
                EnumeratorOptions options;
                std::mutex        mutex;
                dbus::Connection  connection;
        };

        [[nodiscard]]
        grab::Result<void>
        ensure_connected( EnumeratorState& state )
        {
            if( state.connection != nullptr )
            {
                return {};
            }
            auto address = dbus::resolve_bus_address();
            if( !address.has_value() )
            {
                return std::unexpected( std::move( address.error() ) );
            }
            auto connection = dbus::open_connection( *address );
            if( !connection.has_value() )
            {
                return std::unexpected( std::move( connection.error() ) );
            }
            state.connection = std::move( *connection );
            return {};
        }

        [[nodiscard]]
        std::string
        read_name( DBusConnection*    connection,
                   const std::string& destination,
                   const std::string& path )
        {
            const dbus::Message reply =
                call( connection,
                      destination.c_str(),
                      path.c_str(),
                      propertiesInterface,
                      propertiesGetMethod,
                      [&]( DBusMessageIter& iterator )
                      {
                          append_string( iterator, accessibleInterface );
                          append_string( iterator, nameProperty );
                      } );
            return reply == nullptr ? std::string{} : read_variant_string( reply );
        }

        [[nodiscard]]
        std::string
        read_text( DBusConnection*    connection,
                   const std::string& destination,
                   const std::string& path )
        {
            const dbus::Message count_reply =
                call( connection,
                      destination.c_str(),
                      path.c_str(),
                      propertiesInterface,
                      propertiesGetMethod,
                      [&]( DBusMessageIter& iterator )
                      {
                          append_string( iterator, textInterface );
                          append_string( iterator, characterCountProperty );
                      } );
            if( count_reply == nullptr )
            {
                return {};
            }
            const auto count = read_variant_int( count_reply );
            if( !count.has_value() || *count <= 0 )
            {
                return {};
            }

            const std::int32_t  end = *count;
            const dbus::Message reply =
                call( connection,
                      destination.c_str(),
                      path.c_str(),
                      textInterface,
                      getTextMethod,
                      [end]( DBusMessageIter& iterator )
                      {
                          std::int32_t start = 0;
                          static_cast<void>( dbus_message_iter_append_basic(
                              &iterator,
                              DBUS_TYPE_INT32,
                              static_cast<const void*>( &start )
                          ) );
                          std::int32_t stop = end;
                          static_cast<void>( dbus_message_iter_append_basic(
                              &iterator,
                              DBUS_TYPE_INT32,
                              static_cast<const void*>( &stop )
                          ) );
                      } );
            return reply == nullptr ? std::string{} : read_string_reply( reply );
        }

        [[nodiscard]]
        std::string
        read_uri( DBusConnection*    connection,
                  const std::string& destination,
                  const std::string& path )
        {
            const dbus::Message reply =
                call( connection,
                      destination.c_str(),
                      path.c_str(),
                      hyperlinkInterface,
                      getUriMethod,
                      []( DBusMessageIter& iterator )
                      {
                          std::int32_t index = 0;
                          static_cast<void>( dbus_message_iter_append_basic(
                              &iterator,
                              DBUS_TYPE_INT32,
                              static_cast<const void*>( &index )
                          ) );
                      } );
            return reply == nullptr ? std::string{} : read_string_reply( reply );
        }

        // Reads one accessible's attributes over D-Bus into an AtspiAccessible.
        // Each read is independent and best-effort: a failed sub-call leaves that
        // field at its default rather than dropping the whole node, so a single
        // uncooperative interface never blanks an otherwise good harvest.
        [[nodiscard]]
        AtspiAccessible
        build_accessible( DBusConnection*              connection,
                          const std::string&           destination,
                          const std::string&           path,
                          std::uint64_t                node_id,
                          std::optional<std::uint64_t> parent )
        {
            AtspiAccessible accessible;
            accessible.node        = grab::NodeId{ node_id };
            accessible.object_path = path;
            accessible.parent =
                parent.has_value()
                    ? std::optional<grab::NodeId>{ grab::NodeId{ *parent } }
                    : std::nullopt;

            const dbus::Message role_reply = call( connection,
                                                   destination.c_str(),
                                                   path.c_str(),
                                                   accessibleInterface,
                                                   getRoleNameMethod,
                                                   append_none );
            if( role_reply != nullptr )
            {
                accessible.role = role_from_name( read_string_reply( role_reply ) );
            }

            const dbus::Message state_reply = call( connection,
                                                    destination.c_str(),
                                                    path.c_str(),
                                                    accessibleInterface,
                                                    getStateMethod,
                                                    append_none );
            if( state_reply != nullptr )
            {
                accessible.states = read_states( state_reply );
            }

            const dbus::Message     iface_reply = call( connection,
                                                        destination.c_str(),
                                                        path.c_str(),
                                                        accessibleInterface,
                                                        getInterfacesMethod,
                                                        append_none );
            const InterfacePresence presence    = iface_reply == nullptr
                                                    ? InterfacePresence{}
                                                    : read_interfaces( iface_reply );
            accessible.interfaces               = presence.set;

            accessible.name = read_name( connection, destination, path );

            if( presence.component )
            {
                const dbus::Message extents =
                    call( connection,
                          destination.c_str(),
                          path.c_str(),
                          componentInterface,
                          getExtentsMethod,
                          []( DBusMessageIter& iterator )
                          {
                              std::uint32_t coord = coordScreen;
                              static_cast<void>( dbus_message_iter_append_basic(
                                  &iterator,
                                  DBUS_TYPE_UINT32,
                                  static_cast<const void*>( &coord )
                              ) );
                          } );
                if( extents != nullptr )
                {
                    accessible.bounds = read_extents( extents );
                }
            }

            if( presence.text )
            {
                accessible.text_content = read_text( connection, destination, path );
            }

            if( presence.hyperlink )
            {
                accessible.url = read_uri( connection, destination, path );
            }

            return accessible;
        }

        [[nodiscard]]
        grab::Result<std::vector<AtspiAccessible>>
        walk( EnumeratorState& state )
        {
            // A missing accessibility bus is "accessibility is off," not a fault:
            // degrade to an empty tree so the snapshot succeeds empty exactly as
            // it did before this enumerator existed. Only a *present* bus that
            // then misbehaves is worth surfacing, and even then per-node.
            auto connected = ensure_connected( state );
            if( !connected.has_value() )
            {
                return std::vector<AtspiAccessible>{};
            }
            DBusConnection* const        connection = state.connection.get();

            std::vector<AtspiAccessible> nodes;

            // (bus-name, path) -> assigned NodeId, for parent linkage + cycle
            // guard. AT-SPI is a tree, but a hostile provider could form a cycle.
            std::unordered_map<std::string, std::uint64_t> assigned;
            std::uint64_t                                  next_node = 1U;

            struct Pending
            {
                    std::string                  destination;
                    std::string                  path;
                    std::optional<std::uint64_t> parent;
            };

            std::deque<Pending> frontier;

            // Seed with the applications under the registry desktop root.
            const dbus::Message roots = call( connection,
                                              registryDest,
                                              rootPath,
                                              accessibleInterface,
                                              getChildrenMethod,
                                              append_none );
            if( roots == nullptr )
            {
                // Bus is up but the registry did not answer: treat as an empty
                // desktop rather than sinking the snapshot.
                return std::vector<AtspiAccessible>{};
            }
            for( auto& [bus_name, path] : read_object_refs( roots ) )
            {
                frontier.push_back( Pending{
                    .destination = bus_name,
                    .path        = path,
                    .parent      = std::nullopt
                } );
            }

            while( !frontier.empty() )
            {
                if( nodes.size() >= state.options.max_nodes )
                {
                    break;
                }
                const Pending pending = std::move( frontier.front() );
                frontier.pop_front();

                const std::string key = pending.destination + '\n' + pending.path;
                if( assigned.contains( key ) )
                {
                    continue;
                }
                const std::uint64_t node_id = next_node++;
                assigned.emplace( key, node_id );

                nodes.push_back( build_accessible( connection,
                                                   pending.destination,
                                                   pending.path,
                                                   node_id,
                                                   pending.parent ) );

                const dbus::Message children = call( connection,
                                                     pending.destination.c_str(),
                                                     pending.path.c_str(),
                                                     accessibleInterface,
                                                     getChildrenMethod,
                                                     append_none );
                if( children != nullptr )
                {
                    for( auto& [child_name, child_path] : read_object_refs( children ) )
                    {
                        frontier.push_back( Pending{
                            .destination = child_name,
                            .path        = child_path,
                            .parent      = node_id
                        } );
                    }
                }
            }

            return nodes;
        }

    }    // namespace

    AtspiTreeSource::AccessibleEnumerator
    make_dbus_enumerator( EnumeratorOptions options )
    {
        auto state     = std::make_shared<EnumeratorState>();
        state->options = options;
        return [state]() -> grab::Result<std::vector<AtspiAccessible>>
        {
            const std::scoped_lock lock{ state->mutex };
            return walk( *state );
        };
    }

}    // namespace grab::drivers::semantic::atspi
