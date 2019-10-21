#pragma once
#include "common.hpp"
#define Unpaged_Text_Section Section(".unpaged.text")
#define Unpaged_Data_Section Section(".unpaged.data")
#define Unpaged_Bss_Section Section(".unpaged.bss")

static_assert(sizeof(void *) == 8, "Just 64-bit code generation is supported.");

#define _ctx_interrupt_ [[interrupt_context]]
ExportC NoReturn void _kstart(const kernel_start_args *args);

ExportC Unpaged_Text_Section u64 _init_unpaged(const kernel_start_args *args);