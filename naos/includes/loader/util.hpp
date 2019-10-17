#pragma once
#include "../common.hpp"

ExportC void memcpy(void *dst, const void *source, u32 len);
void memzero(void *dst, u32 len);