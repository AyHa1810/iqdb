#ifndef IQDB_TYPES_H
#define IQDB_TYPES_H_N

#include <cstdint>

namespace iqdb {

using imageId = uint32_t;
using postId = uint32_t; // An external (Danbooru/other) post ID. used in server.cpp in place of imageId
using iqdbId = uint32_t; // An internal IQDB image ID. This is only used fopr priority queue in this context.

// The type used for calculating similarity scores during queries, and for
// storing `avgl` values in the `m_info` array.
using Score = float;

}

#endif
