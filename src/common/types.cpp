// src/common/types.cpp
//
// Translation unit kept (rather than header-only) so that the header can be
// cheap to include everywhere without duplicating inline definitions in
// multiple TUs. There are no non-inline definitions today, but keeping the TU
// in place lets us add them later without re-touching every consumer.

#include "tt/common/types.hpp"

namespace tt {
// Intentionally empty. Defined here to anchor the library target's sources.
}  // namespace tt
