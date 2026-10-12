// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <string>

namespace sphinx {

struct SphinxCacheOptions {
  std::string nodes = "127.0.0.1:11211";
  std::chrono::milliseconds timeout{200};
};

void validate_sphinx_options(const SphinxCacheOptions& options);

}  // namespace sphinx
