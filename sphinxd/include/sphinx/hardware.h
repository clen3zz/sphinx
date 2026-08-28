// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace sphinx::hardware {

static constexpr int cache_line_size =
#ifdef __x86_64__
    64;
#else
#error "L1 cache line size is not defined for this architecture."
#endif
}  // namespace sphinx::hardware
