#pragma once
#include "common.hpp"
#include "pkg.hpp"
namespace io
{
void init();

bool send_io_request(request_t *request);
void call_next_io_request_chain(request_t *request);

bool attach_request_chain_device(u32 source_dev, u32 target_dev, request_chain_number_t request_chain);

void remove_request_chain_device(u32 device);

} // namespace io
