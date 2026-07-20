#pragma once

#include "grab/image.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace grab::image
{

    enum class DirCompareMode : std::uint8_t
    {
        Exact,
        Rmse,
        Count,
    };

    struct FileCompareResult
    {
            std::string name;
            bool        in_ref{};
            bool        in_current{};
            double      score{};
            bool        passed{};
    };

    [[nodiscard]]
    grab::Result<double>
    rmse( const Image& a,
          const Image& b );

    [[nodiscard]]
    grab::Result<std::vector<FileCompareResult>>
    compare_dirs( const std::filesystem::path& ref,
                  const std::filesystem::path& current,
                  DirCompareMode               mode,
                  double                       threshold );

}    // namespace grab::image
