#pragma once
#include "common.hpp"
#include "pkg.hpp"
namespace io
{
void init();
void send_io_package();
void send_io_request(request_t *req);
} // namespace io
