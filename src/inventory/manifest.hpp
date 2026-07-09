#pragma once

#include "grab/result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace grab::inventory
{

    struct Entry
    {
            std::string name;
            std::string category;
            std::string module;
            std::string source_file;
            std::string render_method;
            std::string output_path;
            std::string status;
            std::string notes;
    };

    [[nodiscard]]
    grab::Result<void>
    write_manifest( std::string_view   path,
                    std::vector<Entry> entries );

}    // namespace grab::inventory
