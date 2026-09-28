// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include "pebble_host.h"

// Use the compiler runtime's embedded random hook so libstdc++ does not
// require a POSIX getentropy syscall from the separately linked app blob.
inline std::uint64_t gea_runtime_platform_random_u64()
{
	return gea_pebble_host->random_u64();
}
