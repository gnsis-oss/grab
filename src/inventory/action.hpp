#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace grab::inventory
{

    inline constexpr std::uint8_t left_button          = 1U;
    inline constexpr std::uint8_t right_button         = 3U;

    inline constexpr double       menu_header_fy       = 0.0153;
    inline constexpr double       menu_first_item_fy   = 0.0475;
    inline constexpr double       menu_item_spacing_fy = 0.0266;

    inline constexpr char         sample_key_prefix    = '@';

    struct ActivateStep
    {
    };

    struct KeyStep
    {
            std::string keys;
    };

    struct TypeStep
    {
            std::string text;
    };

    struct SleepStep
    {
            double seconds = 0.0;
    };

    struct ClickFracStep
    {
            double       fx     = 0.0;
            double       fy     = 0.0;
            std::uint8_t button = left_button;
    };

    struct ClickOffStep
    {
            int          ox     = 0;
            int          oy     = 0;
            std::uint8_t button = left_button;
    };

    struct MenuOpenStep
    {
            double header_fx = 0.0;
            double header_fy = menu_header_fy;
    };

    struct MenuItemStep
    {
            double header_fx = 0.0;
            double item_fx   = 0.0;
            int    index     = 0;
            double header_fy = menu_header_fy;
    };

    struct RightClickNodeStep
    {
            int ox = 0;
            int oy = 0;
    };

    struct DragCurveStep
    {
            double src_fx = 0.0;
            double src_fy = 0.0;
            double dst_fx = 0.0;
            double dst_fy = 0.0;
    };

    struct DragFracStep
    {
            int    from_ox = 0;
            int    from_oy = 0;
            double to_fx   = 0.0;
            double to_fy   = 0.0;
    };

    struct LoadFileStep
    {
            std::string path;
    };

    struct OpenStreamStep
    {
            std::string source;
    };

    struct SetComboStep
    {
            double      fx = 0.0;
            double      fy = 0.0;
            std::string value;
    };

    using Step = std::variant<ActivateStep,
                              KeyStep,
                              TypeStep,
                              SleepStep,
                              ClickFracStep,
                              ClickOffStep,
                              MenuOpenStep,
                              MenuItemStep,
                              RightClickNodeStep,
                              DragCurveStep,
                              DragFracStep,
                              LoadFileStep,
                              OpenStreamStep,
                              SetComboStep>;

    [[nodiscard]]
    inline Step
    activate()
    {
        return Step{ ActivateStep{} };
    }

    [[nodiscard]]
    inline Step
    key( std::string_view keys )
    {
        return Step{ KeyStep{ .keys = std::string{ keys } } };
    }

    [[nodiscard]]
    inline Step
    type( std::string_view text )
    {
        return Step{ TypeStep{ .text = std::string{ text } } };
    }

    [[nodiscard]]
    inline Step
    sleep( double seconds )
    {
        return Step{ SleepStep{ .seconds = seconds } };
    }

    [[nodiscard]]
    inline Step
    click( double       fx,
           double       fy,
           std::uint8_t button = left_button )
    {
        return Step{
            ClickFracStep{ .fx = fx, .fy = fy, .button = button }
        };
    }

    [[nodiscard]]
    inline Step
    rclick( double fx,
            double fy )
    {
        return click( fx, fy, right_button );
    }

    [[nodiscard]]
    inline Step
    click_off( int          ox,
               int          oy,
               std::uint8_t button = left_button )
    {
        return Step{
            ClickOffStep{ .ox = ox, .oy = oy, .button = button }
        };
    }

    [[nodiscard]]
    inline Step
    right_click_node( int ox,
                      int oy )
    {
        return Step{
            RightClickNodeStep{ .ox = ox, .oy = oy }
        };
    }

    [[nodiscard]]
    inline Step
    menu_open( double header_fx )
    {
        return Step{ MenuOpenStep{ .header_fx = header_fx } };
    }

    [[nodiscard]]
    inline Step
    menu_item( double header_fx,
               double item_fx,
               int    index )
    {
        return Step{
            MenuItemStep{ .header_fx = header_fx, .item_fx = item_fx, .index = index }
        };
    }

    [[nodiscard]]
    inline Step
    drag( double src_fx,
          double src_fy,
          double dst_fx,
          double dst_fy )
    {
        return Step{
            DragCurveStep{
                          .src_fx = src_fx,
                          .src_fy = src_fy,
                          .dst_fx = dst_fx,
                          .dst_fy = dst_fy
            }
        };
    }

    [[nodiscard]]
    inline Step
    drag_frac( int    from_ox,
               int    from_oy,
               double to_fx,
               double to_fy )
    {
        return Step{
            DragFracStep{
                         .from_ox = from_ox,
                         .from_oy = from_oy,
                         .to_fx   = to_fx,
                         .to_fy   = to_fy
            }
        };
    }

    [[nodiscard]]
    inline Step
    load( std::string_view key )
    {
        std::string path;
        path.reserve( key.size() + 1U );
        path.push_back( sample_key_prefix );
        path.append( key );
        return Step{ LoadFileStep{ .path = std::move( path ) } };
    }

    [[nodiscard]]
    inline Step
    load_file( std::string_view path )
    {
        return Step{ LoadFileStep{ .path = std::string{ path } } };
    }

    [[nodiscard]]
    inline Step
    open_stream( std::string_view source )
    {
        return Step{ OpenStreamStep{ .source = std::string{ source } } };
    }

    [[nodiscard]]
    inline Step
    set_combo( double           fx,
               double           fy,
               std::string_view value )
    {
        return Step{
            SetComboStep{ .fx = fx, .fy = fy, .value = std::string{ value } }
        };
    }

}    // namespace grab::inventory
