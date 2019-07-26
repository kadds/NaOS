#pragma once
#include "common.hpp"
#define Unpaged_Text_Section Section(".unpaged.text")
#define Unpaged_Data_Section Section(".unpaged.data")
#define Unpaged_Bss_Section Section(".unpaged.bss")
extern volatile char _bss_end_addr[];
extern volatile char _bss_start_addr[];
extern volatile char _bss_unpaged_end_addr[];
extern volatile char _bss_unpaged_start_addr[];
