#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// JSON bytes in, a typed Sequence out.
//
//   JSON bytes
//     -> nlohmann::json
//     -> op string  -> grab::command_kind()   unknown -> error naming the op
//     -> payload    -> one parser per CommandKind
//     -> ids + after -> Sequence::build()     which owns graph and cycle checks
//     -> Result<Sequence>
//
// The loader rejects what it cannot BUILD — unknown op, an op with no payload
// struct, a dangling `after`, a duplicate label, a self-edge, a cycle. Those
// are not policy: a cycle does not fail validation, it fails to terminate.
// Policy lives in validate(), which is a declared pass-through seam.
//
// Errors carry the step's label AND a JSON pointer (/steps/3/after/0). These
// files are hand-written or LLM-generated; "invalid sequence" helps neither.
//
// PHASE 0: declaration only. The interpreter unit implements it.

#include "grab/result.hpp"
#include "kernel/sequence/sequence.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace grab::kernel::sequence
{

    [[nodiscard]]
    grab::Result<Sequence>
    parse( std::string_view json );

    [[nodiscard]]
    grab::Result<Sequence>
    load( const std::filesystem::path& path );

    // The loader backwards. load -> to_json -> load must yield an identical
    // Sequence, ids included — which works only because ids are positional and
    // therefore never written into the document.
    [[nodiscard]]
    grab::Result<std::string>
    to_json( const Sequence& sequence );

}    // namespace grab::kernel::sequence
