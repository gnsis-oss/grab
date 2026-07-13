#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/result.hpp"
#include "grab/space.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace grab::kernel
{

    struct TargetId
    {
            std::uint64_t value{};
            friend auto
            operator<=>( const TargetId&,
                         const TargetId& ) = default;
    };

    enum class TargetGrade : std::uint8_t
    {
        Application,
        Window,
    };

    struct AliasAuthority
    {
            std::string value;
            friend auto
            operator<=>( const AliasAuthority&,
                         const AliasAuthority& ) = default;
    };

    struct NativeAliasId
    {
            std::string value;
            friend auto
            operator<=>( const NativeAliasId&,
                         const NativeAliasId& ) = default;
    };

    enum class AliasConfidence : std::uint8_t
    {
        Candidate,
        Exact,
    };

    enum class AliasValidity : std::uint8_t
    {
        Active,
        Inactive,
    };

    struct AliasEdge
    {
            AliasAuthority  authority{};
            NativeAliasId   native_id{};
            AliasConfidence confidence{ AliasConfidence::Candidate };
            AliasValidity   validity{ AliasValidity::Active };
            friend auto
            operator<=>( const AliasEdge&,
                         const AliasEdge& ) = default;
    };

    struct TargetObservation
    {
            TargetGrade                  grade{ TargetGrade::Window };
            std::optional<AliasEdge>     alias;
            std::string                  title;
            std::optional<std::uint32_t> pid;
            std::optional<SpaceRect>     bounds;
    };

    // A value snapshot of one durable target. Returning records by value keeps
    // registry reads safe when observations arrive concurrently.
    struct TargetRecord
    {
            TargetId                       id{};
            TargetGrade                    grade{ TargetGrade::Window };
            std::vector<AliasEdge>         aliases;
            std::vector<TargetObservation> observations;
    };

    class TargetRegistry final
    {
        public:

            TargetRegistry()                        = default;
            ~TargetRegistry()                       = default;

            TargetRegistry( const TargetRegistry& ) = delete;
            TargetRegistry&
            operator=( const TargetRegistry& ) = delete;
            TargetRegistry( TargetRegistry&& ) = delete;
            TargetRegistry&
            operator=( TargetRegistry&& ) = delete;

            // Only an exact, active authority/native-id pair is an identity
            // fuse. Titles, PIDs, geometry, candidate aliases, and inactive
            // aliases are retained as evidence but never merge targets.
            [[nodiscard]]
            Result<TargetId>
            observe( TargetObservation observation );

            // Registers an exact bridge contributed by another authority
            // without re-evaluating heuristic target evidence.
            [[nodiscard]]
            Result<void>
            attach_alias( TargetId  target,
                          AliasEdge alias );

            // Ends the active validity interval for an authority/native-id
            // edge. Reuse of that native id creates a fresh durable target.
            [[nodiscard]]
            Result<void>
            invalidate_alias( const AliasAuthority& authority,
                              const NativeAliasId&  native_id );

            [[nodiscard]]
            Result<TargetRecord>
            target( TargetId id ) const;

            [[nodiscard]]
            Result<std::vector<TargetRecord>>
            targets() const;

            [[nodiscard]]
            std::size_t
            size() const;

        private:

            struct AliasKey
            {
                    AliasAuthority authority{};
                    NativeAliasId  native_id{};
                    friend auto
                    operator<=>( const AliasKey&,
                                 const AliasKey& ) = default;
            };

            [[nodiscard]]
            static bool
                               is_fusing( const AliasEdge& alias ) noexcept;

            mutable std::mutex mutex_;
            std::map<TargetId, TargetRecord> targets_;
            std::map<AliasKey, TargetId>     exact_active_aliases_;
            std::uint64_t                    next_id_{ 1U };
    };

}    // namespace grab::kernel
