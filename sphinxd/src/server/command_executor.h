// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "message.h"

#include <sphinx/logmem.h>
#include <sphinx/stats.h>

#include <string>
namespace sphinx::server {

struct ExecutionResult
{
  std::string payload;
};
// Execute on the owning worker; Command carries response metadata.
ExecutionResult
execute_command(sphinx::logmem::Log& log,
                sphinx::stats::ServerStats& stats,
                const Command& command);
} // namespace sphinx::server
