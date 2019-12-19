#pragma once
#include "../kernel.hpp"
#include "common.hpp"

/// \brief Architecture specific space.
///
/// The initialization function of the architecture is here.
/// Called by kmain
namespace arch
{
ExportC Unpaged_Text_Section void temp_init(const kernel_start_args *args);
void init(const kernel_start_args *args);
/// init before task created
void last_init();

void init_drivers();
} // namespace arch