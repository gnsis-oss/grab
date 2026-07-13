#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "grab/ui.hpp"
#include "kernel/graph/target_registry.hpp"
#include "spi/tree_source.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace grab::drivers::semantic::atspi
{

    enum class AtspiRole : std::uint16_t
    {
        Unknown,
        Application,
        Window,
        Frame,
        Document,
        Dialog,
        Alert,
        Panel,
        Section,
        Button,
        PushButton,
        ToggleButton,
        CheckBox,
        RadioButton,
        Entry,
        PasswordText,
        Text,
        Paragraph,
        Heading,
        Link,
        Image,
        List,
        ListItem,
        Table,
        TableCell,
        Menu,
        MenuBar,
        MenuItem,
        PageTab,
        PageTabList,
        Slider,
        SpinButton,
        ComboBox,
    };

    // The interface set is deliberately detached from libatspi/DBus types so
    // role/facet mapping is deterministic and testable without an a11y bus.
    struct AtspiInterfaceSet
    {
            bool action{};
            bool text{};
            bool value{};
            bool selection{};
    };

    struct AtspiAccessible
    {
            grab::NodeId                   node{};
            grab::NodeGeneration           generation{ 1U };
            AtspiRole                      role{ AtspiRole::Unknown };
            AtspiInterfaceSet              interfaces{};
            std::string                    object_path;
            std::optional<grab::NodeId>    parent;
            std::string                    name;
            std::string                    title;
            std::string                    text_content;
            std::uint32_t                  states{};
            std::optional<std::uint32_t>   pid;
            std::optional<grab::SpaceRect> bounds;
            // Populated only from a toolkit-provided AT-SPI window bridge.
            std::optional<std::uint32_t>   x11_window;
    };

    [[nodiscard]]
    grab::RoleId
    map_role( AtspiRole role ) noexcept;

    [[nodiscard]]
    grab::UiNodeRecord
    map_accessible( const AtspiAccessible& accessible,
                    grab::RuntimeId        runtime,
                    std::uint64_t          revision );

    // A title/PID-only observation receives Candidate confidence. An Exact
    // alias is emitted only when x11_window and its bridge authority are both
    // supplied by the toolkit/runtime composition layer.
    [[nodiscard]]
    grab::Result<grab::kernel::TargetId>
    observe_atspi_target( grab::kernel::TargetRegistry& registry,
                          const AtspiAccessible&        accessible,
                          std::optional<std::string>    x11_alias_authority =
                              std::nullopt );

    class AtspiTreeSource final : public grab::spi::TreeSource
    {
        public:

            using AccessibleEnumerator =
                std::function<grab::Result<std::vector<AtspiAccessible>>()>;

            AtspiTreeSource( grab::RuntimeId               runtime,
                             grab::kernel::TargetRegistry& targets,
                             AccessibleEnumerator          enumerate_accessibles = {},
                             std::optional<std::string>    x11_alias_authority =
                                 std::nullopt );

            [[nodiscard]]
            grab::Result<grab::UiSnapshot>
            snapshot( std::uint32_t                 tree,
                      const grab::OperationContext& context ) override;

            [[nodiscard]]
            grab::Result<std::optional<grab::spi::UiUpdate>>
            next_update( const grab::OperationContext& context ) override;

        private:

            static constexpr std::uint32_t                 firstTree  = 1U;
            static constexpr std::uint32_t                 firstEpoch = 1U;

            grab::RuntimeId                                runtime_{};
            grab::kernel::TargetRegistry*                  targets_{};
            AccessibleEnumerator                           enumerate_accessibles_;
            std::optional<std::string>                     x11_alias_authority_;
            std::map<grab::NodeId, grab::kernel::TargetId> target_bindings_;
            std::uint64_t                                  revision_{};
    };

}    // namespace grab::drivers::semantic::atspi
