#pragma once

#include "input/input_sink.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace grab::test
{

    struct Move
    {
            grab::input::Point p;

            [[nodiscard]]
            friend bool
            operator==( const Move& lhs,
                        const Move& rhs ) = default;
    };

    struct Button
    {
            std::uint8_t code;
            bool         press;
            bool         clear_modifiers;

            [[nodiscard]]
            friend bool
            operator==( const Button& lhs,
                        const Button& rhs ) = default;
    };

    struct Sync
    {
            [[nodiscard]]
            friend bool
            operator==( Sync lhs,
                        Sync rhs ) = default;
    };

    struct Wait
    {
            std::uint32_t millis;

            [[nodiscard]]
            friend bool
            operator==( const Wait& lhs,
                        const Wait& rhs ) = default;
    };

    struct TypeText
    {
            std::string text;

            [[nodiscard]]
            friend bool
            operator==( const TypeText& lhs,
                        const TypeText& rhs ) = default;
    };

    struct Key
    {
            std::string keysym;

            [[nodiscard]]
            friend bool
            operator==( const Key& lhs,
                        const Key& rhs ) = default;
    };

    struct Activate
    {
            [[nodiscard]]
            friend bool
            operator==( Activate lhs,
                        Activate rhs ) = default;
    };

    using Op = std::variant<Move, Button, Sync, Wait, TypeText, Key, Activate>;

    class FakeInputSink final : public grab::input::InputSink
    {
        public:

            [[nodiscard]]
            const std::vector<Op>&
            ops() const noexcept
            {
                return recorded_ops;
            }

            void
            move( grab::input::Point p ) override
            {
                recorded_ops.emplace_back( Move{ .p = p } );
            }

            void
            button( std::uint8_t code,
                    bool         press,
                    bool         clear_modifiers ) override
            {
                recorded_ops.emplace_back( Button{
                    .code            = code,
                    .press           = press,
                    .clear_modifiers = clear_modifiers,
                } );
            }

            void
            sync() override
            {
                recorded_ops.emplace_back( Sync{} );
            }

            void
            wait( std::uint32_t millis ) override
            {
                recorded_ops.emplace_back( Wait{ .millis = millis } );
            }

            void
            type_text( std::string_view utf8 ) override
            {
                recorded_ops.emplace_back( TypeText{ .text = std::string{ utf8 } } );
            }

            void
            key( std::string_view keysym ) override
            {
                recorded_ops.emplace_back( Key{ .keysym = std::string{ keysym } } );
            }

            void
            activate() override
            {
                recorded_ops.emplace_back( Activate{} );
            }

        private:

            std::vector<Op> recorded_ops;
    };

}    // namespace grab::test
