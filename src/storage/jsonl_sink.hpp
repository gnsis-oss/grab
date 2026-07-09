#ifndef STORAGE_JSONL_SINK_HPP
#define STORAGE_JSONL_SINK_HPP

#include "grab/event.hpp"
#include "grab/result.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>

namespace grab::storage
{

    struct JsonlOptions
    {
            std::filesystem::path dir;
            std::size_t           buffer_limit = 100U;
            std::size_t           max_files    = 30U;
            std::size_t           max_disk_mb  = 500U;
    };

    class JsonlSink
    {
        public:

            [[nodiscard]]
            static grab::Result<JsonlSink>
            open( JsonlOptions options );

            ~JsonlSink();

            JsonlSink( const JsonlSink& ) = delete;
            JsonlSink&
            operator=( const JsonlSink& ) = delete;
            JsonlSink( JsonlSink&& ) noexcept;
            JsonlSink&
            operator=( JsonlSink&& ) noexcept;

            [[nodiscard]]
            grab::Result<void>
            write( const grab::Event& event );

            [[nodiscard]]
            grab::Result<void>
            flush();

            void
            close() noexcept;

        private:

            class Impl;

            explicit JsonlSink( std::unique_ptr<Impl> impl ) noexcept;

            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab::storage

#endif    // STORAGE_JSONL_SINK_HPP
