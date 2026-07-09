#ifndef GRAB_SCREEN_WORKFLOW_HPP
#define GRAB_SCREEN_WORKFLOW_HPP

#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "image/compare.hpp"
#include "notify/notifier.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace grab::screen
{

    struct BatchItem
    {
            std::vector<std::string> wm_class_candidates;
            std::string              out_path;
    };

    struct BatchResult
    {
            std::uint32_t            captured = 0U;
            std::vector<std::string> misses;
    };

    [[nodiscard]]
    grab::Result<BatchResult>
    batch_capture( grab::Screen&                 screen,
                   const std::vector<BatchItem>& items );

    [[nodiscard]]
    grab::Result<grab::image::DiffResult>
    compare_files( const std::string&      path_a,
                   const std::string&      path_b,
                   grab::notify::Notifier* notifier = nullptr );

    [[nodiscard]]
    grab::Result<std::uint32_t>
    watch_capture( grab::Screen&                   screen,
                   const std::vector<std::string>& wm_class_candidates,
                   const std::string&              out_path,
                   const std::function<bool()>&    should_stop );

}    // namespace grab::screen

#endif    // GRAB_SCREEN_WORKFLOW_HPP
