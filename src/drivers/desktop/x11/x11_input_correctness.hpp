#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/result.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

struct xcb_connection_t;

namespace grab::drivers::desktop::x11
{

    class ModifierState
    {
        public:

            virtual ~ModifierState() = default;

            [[nodiscard]]
            virtual bool
            held( std::uint8_t keycode ) const = 0;

            [[nodiscard]]
            virtual bool
            set( std::uint8_t keycode,
                 bool         press ) = 0;
    };

    class ModifierGuard final
    {
        public:

            ModifierGuard( ModifierState&                state,
                           std::span<const std::uint8_t> modifier_keycodes );
            ~ModifierGuard();

            ModifierGuard( const ModifierGuard& ) = delete;
            ModifierGuard&
            operator=( const ModifierGuard& ) = delete;
            ModifierGuard( ModifierGuard&& )  = delete;
            ModifierGuard&
            operator=( ModifierGuard&& ) = delete;

            [[nodiscard]]
            bool
            release_succeeded() const noexcept;

            [[nodiscard]]
            bool
            restore();

        private:

            ModifierState*         state_{};
            std::array<bool, 256U> restore_{};
            bool                   release_succeeded_ = true;
            bool                   active_            = true;
    };

    class SeatLane final
    {
        public:

            using Token = std::unique_lock<std::mutex>;

            [[nodiscard]]
            Token
            acquire();

        private:

            std::mutex mutex_;
    };

    struct ScratchKeycodeMapping
    {
            std::uint8_t               keycode{};
            std::vector<std::uint32_t> keysyms;
    };

    class ScratchKeycodeBackend
    {
        public:

            virtual ~ScratchKeycodeBackend() = default;

            [[nodiscard]]
            virtual grab::Result<std::vector<ScratchKeycodeMapping>>
            mappings() = 0;

            [[nodiscard]]
            virtual grab::Result<void>
            replace( std::uint8_t                   keycode,
                     std::span<const std::uint32_t> keysyms ) = 0;

            [[nodiscard]]
            virtual grab::Result<void>
            fence() = 0;
    };

    [[nodiscard]]
    std::optional<std::uint8_t>
    find_unused_keycode( std::span<const ScratchKeycodeMapping> mappings );

    class ScratchKeycodePool final
    {
        public:

            class Loan final
            {
                public:

                    ~Loan();

                    Loan( const Loan& ) = delete;
                    Loan&
                    operator=( const Loan& ) = delete;
                    Loan( Loan&& other ) noexcept;
                    Loan&
                    operator=( Loan&& other ) noexcept;

                    [[nodiscard]]
                    std::uint8_t
                    keycode() const noexcept;

                    [[nodiscard]]
                    grab::Result<void>
                    restore();

                private:

                    friend class ScratchKeycodePool;

                    Loan( ScratchKeycodePool& pool,
                          std::uint32_t       keysym,
                          std::uint8_t        keycode ) noexcept;

                    ScratchKeycodePool* pool_{};
                    std::uint32_t       keysym_{};
                    std::uint8_t        keycode_{};
            };

            explicit ScratchKeycodePool( xcb_connection_t* connection );
            explicit ScratchKeycodePool(
                std::unique_ptr<ScratchKeycodeBackend> backend
            ) noexcept;
            ~ScratchKeycodePool();

            ScratchKeycodePool( const ScratchKeycodePool& ) = delete;
            ScratchKeycodePool&
            operator=( const ScratchKeycodePool& )     = delete;
            ScratchKeycodePool( ScratchKeycodePool&& ) = delete;
            ScratchKeycodePool&
            operator=( ScratchKeycodePool&& ) = delete;

            [[nodiscard]]
            grab::Result<Loan>
            loan( std::uint32_t keysym );

        private:

            struct Entry
            {
                    std::uint32_t              keysym{};
                    std::uint8_t               keycode{};
                    std::size_t                references{};
                    std::vector<std::uint32_t> original;
            };

            [[nodiscard]]
            grab::Result<void>
                                                   release( std::uint32_t keysym,
                                                            std::uint8_t  keycode );

            std::unique_ptr<ScratchKeycodeBackend> backend_;
            std::vector<Entry>                     entries_;
            std::mutex                             mutex_;
    };

}    // namespace grab::drivers::desktop::x11
