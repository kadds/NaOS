#pragma once
#include "../kernel.hpp"
#include "common.hpp"

namespace arch
{
ExportC Unpaged_Text_Section void temp_init(const kernel_start_args *args);
void init(const kernel_start_args *args);
} // namespace arch