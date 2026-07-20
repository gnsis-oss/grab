#include "codec/png.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "image/compare.hpp"
#include "image/compare_dirs.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace grab::image
{
    namespace
    {

        constexpr std::string_view pngExtension  = ".png";
        constexpr std::string_view errorPrefix   = "directory comparison: ";
        constexpr std::string_view pathSeparator = ": ";
        constexpr std::size_t      singleByte    = 1U;

        struct ImageLayout
        {
                std::size_t bytes_per_pixel{};
                std::size_t row_bytes{};
        };

        struct PngFile
        {
                std::string           name;
                std::filesystem::path path;
        };

        [[nodiscard]]
        std::unexpected<grab::Error>
        invalid_argument_error( std::string message )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ errorPrefix } + std::move( message ) );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        filesystem_error( std::string_view             operation,
                          const std::filesystem::path& path,
                          const std::error_code&       error )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               std::string{ errorPrefix } +
                                   std::string{ operation } +
                                   std::string{ pathSeparator } +
                                   path.string() +
                                   std::string{ pathSeparator } +
                                   error.message() );
        }

        [[nodiscard]]
        bool
        multiply_overflows_size( std::size_t left,
                                 std::size_t right ) noexcept
        {
            return right !=
                   0U &&
                   left > ( std::numeric_limits<std::size_t>::max() / right );
        }

        [[nodiscard]]
        bool
        add_overflows_size( std::size_t left,
                            std::size_t right ) noexcept
        {
            return left > ( std::numeric_limits<std::size_t>::max() - right );
        }

        [[nodiscard]]
        std::byte
        byte_at( std::span<const std::byte> bytes,
                 std::size_t                offset )
        {
            return bytes.subspan( offset, singleByte ).front();
        }

        [[nodiscard]]
        grab::Result<ImageLayout>
        validate_layout( const Image& image )
        {
            const auto bytes_per_pixel =
                static_cast<std::size_t>( grab::bytes_per_pixel( image.format ) );
            const auto width = static_cast<std::size_t>( image.width );
            if( multiply_overflows_size( width, bytes_per_pixel ) )
            {
                return invalid_argument_error( "row byte count overflows size_t" );
            }

            const auto row_bytes = width * bytes_per_pixel;
            if( image.height == 0U )
            {
                return ImageLayout{
                    .bytes_per_pixel = bytes_per_pixel,
                    .row_bytes       = row_bytes,
                };
            }

            const auto stride = static_cast<std::size_t>( image.stride );
            if( stride < row_bytes )
            {
                return invalid_argument_error( "stride is smaller than the pixel row" );
            }

            const auto last_row = static_cast<std::size_t>( image.height - 1U );
            if( multiply_overflows_size( last_row, stride ) )
            {
                return invalid_argument_error( "row offset overflows size_t" );
            }

            const auto last_row_offset = last_row * stride;
            if( add_overflows_size( last_row_offset, row_bytes ) )
            {
                return invalid_argument_error( "pixel buffer size overflows size_t" );
            }
            if( image.pixels.size() < last_row_offset + row_bytes )
            {
                return invalid_argument_error(
                    "pixel buffer is shorter than image rows"
                );
            }

            return ImageLayout{
                .bytes_per_pixel = bytes_per_pixel,
                .row_bytes       = row_bytes,
            };
        }

        [[nodiscard]]
        grab::Result<ImageLayout>
        validate_pair( const Image& a,
                       const Image& b )
        {
            if( a.width != b.width || a.height != b.height )
            {
                return invalid_argument_error( "image dimensions differ" );
            }
            if( a.format != b.format )
            {
                return invalid_argument_error( "image formats differ" );
            }

            auto a_layout = validate_layout( a );
            if( !a_layout.has_value() )
            {
                return std::unexpected( std::move( a_layout.error() ) );
            }

            auto b_layout = validate_layout( b );
            if( !b_layout.has_value() )
            {
                return std::unexpected( std::move( b_layout.error() ) );
            }

            return *a_layout;
        }

        [[nodiscard]]
        grab::Result<std::vector<std::byte>>
        read_binary_file( const std::filesystem::path& path )
        {
            std::ifstream stream{ path, std::ios::binary };
            if( !stream )
            {
                return grab::fail(
                    grab::ErrorCode::ProviderFailed,
                    std::string{ errorPrefix } + "failed to open PNG: " + path.string()
                );
            }

            const std::vector<char> input_bytes{
                std::istreambuf_iterator<char>{ stream },
                std::istreambuf_iterator<char>{}
            };
            if( stream.bad() )
            {
                return grab::fail(
                    grab::ErrorCode::ProviderFailed,
                    std::string{ errorPrefix } + "failed to read PNG: " + path.string()
                );
            }

            std::vector<std::byte> bytes;
            bytes.reserve( input_bytes.size() );
            for( const char value : input_bytes )
            {
                bytes.push_back(
                    static_cast<std::byte>( static_cast<unsigned char>( value ) )
                );
            }
            return bytes;
        }

        [[nodiscard]]
        grab::Result<Image>
        load_png( const std::filesystem::path& path )
        {
            auto bytes = read_binary_file( path );
            if( !bytes.has_value() )
            {
                return std::unexpected( std::move( bytes.error() ) );
            }

            auto image = grab::codec::decode_png( *bytes );
            if( !image.has_value() )
            {
                return std::unexpected( std::move( image.error() ) );
            }
            return image;
        }

        [[nodiscard]]
        grab::Result<std::vector<PngFile>>
        collect_png_files( const std::filesystem::path& directory )
        {
            std::vector<PngFile>                files;
            std::error_code                     error;
            std::filesystem::directory_iterator iterator{ directory, error };
            if( error )
            {
                return filesystem_error( "scan", directory, error );
            }

            const std::filesystem::directory_iterator end;
            while( iterator != end )
            {
                const std::filesystem::path path = iterator->path();
                const bool regular_file          = iterator->is_regular_file( error );
                if( error )
                {
                    return filesystem_error( "inspect", path, error );
                }
                if( regular_file && path.extension() == pngExtension )
                {
                    files.push_back( PngFile{
                        .name = path.filename().string(),
                        .path = path,
                    } );
                }

                iterator.increment( error );
                if( error )
                {
                    return filesystem_error( "scan", directory, error );
                }
            }

            std::ranges::sort( files, {}, &PngFile::name );
            return files;
        }

        [[nodiscard]]
        grab::Result<void>
        validate_options( DirCompareMode mode,
                          double         threshold )
        {
            if( mode != DirCompareMode::Exact && mode != DirCompareMode::Rmse )
            {
                return invalid_argument_error( "invalid directory comparison mode" );
            }
            if( mode ==
                DirCompareMode::Rmse &&
                ( !std::isfinite( threshold ) || threshold < 0.0 ) )
            {
                return invalid_argument_error(
                    "RMSE threshold must be finite and non-negative"
                );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<FileCompareResult>
        compare_matched_file( const PngFile& ref,
                              const PngFile& current,
                              DirCompareMode mode,
                              double         threshold )
        {
            auto ref_image = load_png( ref.path );
            if( !ref_image.has_value() )
            {
                return std::unexpected( std::move( ref_image.error() ) );
            }

            auto current_image = load_png( current.path );
            if( !current_image.has_value() )
            {
                return std::unexpected( std::move( current_image.error() ) );
            }

            if( mode == DirCompareMode::Exact )
            {
                auto result = compare( *ref_image, *current_image );
                if( !result.has_value() )
                {
                    return std::unexpected( std::move( result.error() ) );
                }
                return FileCompareResult{
                    .name       = ref.name,
                    .in_ref     = true,
                    .in_current = true,
                    .score      = static_cast<double>( result->diff_pixels ),
                    .passed     = result->diff_pixels == 0U,
                };
            }

            auto score = rmse( *ref_image, *current_image );
            if( !score.has_value() )
            {
                return std::unexpected( std::move( score.error() ) );
            }
            return FileCompareResult{
                .name       = ref.name,
                .in_ref     = true,
                .in_current = true,
                .score      = *score,
                .passed     = *score <= threshold,
            };
        }

        [[nodiscard]]
        FileCompareResult
        missing_file_result( const PngFile& file,
                             bool           in_ref )
        {
            return FileCompareResult{
                .name       = file.name,
                .in_ref     = in_ref,
                .in_current = !in_ref,
                .score      = 0.0,
                .passed     = false,
            };
        }

    }    // namespace

    grab::Result<double>
    rmse( const Image& a,
          const Image& b )
    {
        auto layout = validate_pair( a, b );
        if( !layout.has_value() )
        {
            return std::unexpected( std::move( layout.error() ) );
        }

        if( a.width == 0U || a.height == 0U )
        {
            return 0.0;
        }

        long double                      squared_error = 0.0L;
        const std::span<const std::byte> a_pixels{ a.pixels };
        const std::span<const std::byte> b_pixels{ b.pixels };
        for( std::uint32_t y = 0U; y < a.height; ++y )
        {
            const auto a_offset =
                static_cast<std::size_t>( y ) * static_cast<std::size_t>( a.stride );
            const auto b_offset =
                static_cast<std::size_t>( y ) * static_cast<std::size_t>( b.stride );
            const auto a_row = a_pixels.subspan( a_offset, layout->row_bytes );
            const auto b_row = b_pixels.subspan( b_offset, layout->row_bytes );

            for( std::size_t index = 0U; index < layout->row_bytes; ++index )
            {
                const auto a_value = static_cast<long double>(
                    std::to_integer<unsigned int>( byte_at( a_row, index ) )
                );
                const auto b_value = static_cast<long double>(
                    std::to_integer<unsigned int>( byte_at( b_row, index ) )
                );
                const long double delta  = a_value - b_value;
                squared_error           += delta * delta;
            }
        }

        const auto sample_count = static_cast<long double>( a.width ) *
                                  static_cast<long double>( a.height ) *
                                  static_cast<long double>( layout->bytes_per_pixel );
        return static_cast<double>( std::sqrt( squared_error / sample_count ) );
    }

    grab::Result<std::vector<FileCompareResult>>
    compare_dirs( const std::filesystem::path& ref,
                  const std::filesystem::path& current,
                  DirCompareMode               mode,
                  double                       threshold )
    {
        auto valid_options = validate_options( mode, threshold );
        if( !valid_options.has_value() )
        {
            return std::unexpected( std::move( valid_options.error() ) );
        }

        auto ref_files = collect_png_files( ref );
        if( !ref_files.has_value() )
        {
            return std::unexpected( std::move( ref_files.error() ) );
        }

        auto current_files = collect_png_files( current );
        if( !current_files.has_value() )
        {
            return std::unexpected( std::move( current_files.error() ) );
        }

        std::vector<FileCompareResult> results;
        std::size_t                    ref_index     = 0U;
        std::size_t                    current_index = 0U;
        while( ref_index < ref_files->size() || current_index < current_files->size() )
        {
            if( current_index ==
                current_files->size() ||
                ( ref_index <
                  ref_files->size() &&
                  ref_files->at( ref_index ).name <
                  current_files->at( current_index ).name ) )
            {
                results.push_back( missing_file_result( ref_files->at( ref_index ),
                                                        true ) );
                ++ref_index;
                continue;
            }

            if( ref_index ==
                ref_files->size() ||
                current_files->at( current_index ).name <
                ref_files->at( ref_index ).name )
            {
                results.push_back(
                    missing_file_result( current_files->at( current_index ), false )
                );
                ++current_index;
                continue;
            }

            auto result = compare_matched_file( ref_files->at( ref_index ),
                                                current_files->at( current_index ),
                                                mode,
                                                threshold );
            if( !result.has_value() )
            {
                return std::unexpected( std::move( result.error() ) );
            }
            results.push_back( std::move( *result ) );
            ++ref_index;
            ++current_index;
        }

        return results;
    }

}    // namespace grab::image
