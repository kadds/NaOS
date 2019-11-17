#pragma once

namespace util
{
int strcmp(const char *str1, const char *str2);
int strcopy(char *dst, const char *src, int max_len);
void strcopy(char *dst, const char *src);

int strlen(const char *str);

} // namespace util
