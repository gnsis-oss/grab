#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace grab
{

    template<typename Enum>
    struct EnumEntry
    {
            constexpr EnumEntry( Enum             entry_value,
                                 std::string_view entry_text ) noexcept :
                value( entry_value ),
                text( entry_text )
            {
            }

            Enum             value;
            std::string_view text;
    };

    template<typename Enum>
    [[nodiscard]]
    constexpr EnumEntry<Enum>
    enum_entry( Enum             value,
                std::string_view text ) noexcept
    {
        return EnumEntry<Enum>{ value, text };
    }

    template<typename Enum, std::size_t Size>
    struct EnumTable
    {
            std::array<EnumEntry<Enum>, Size> entries;

            [[nodiscard]]
            constexpr std::size_t
            size() const noexcept
            {
                return entries.size();
            }

            [[nodiscard]]
            constexpr std::string_view
            text_of( Enum             value,
                     std::string_view fallback ) const noexcept
            {
                for( const auto& entry : entries )
                {
                    if( entry.value == value )
                    {
                        return entry.text;
                    }
                }
                return fallback;
            }

            [[nodiscard]]
            constexpr std::optional<Enum>
            value_of( std::string_view text ) const noexcept
            {
                for( const auto& entry : entries )
                {
                    if( entry.text == text )
                    {
                        return entry.value;
                    }
                }
                return std::nullopt;
            }
    };

    template<typename Enum,
             std::size_t Size>
    EnumTable( std::array<EnumEntry<Enum>,
                          Size> ) -> EnumTable<Enum,
                                               Size>;

    template<typename Enum,
             std::size_t Size>
    [[nodiscard]]
    constexpr bool
    enum_table_has_count( const EnumTable<Enum,
                                          Size>& table,
                          std::size_t            count ) noexcept
    {
        return table.size() == count;
    }

    template<typename Enum,
             std::size_t Size>
    [[nodiscard]]
    constexpr bool
    enum_table_has_count( const EnumTable<Enum,
                                          Size>& table,
                          Enum                   count ) noexcept
    {
        return enum_table_has_count( table, static_cast<std::size_t>( count ) );
    }

}    // namespace grab
