#pragma once

namespace util
{

int strcmp(const char *str1, const char *str2);

///\brief copy src to dst (include \0) same as 'strcopy(char *, const char *)'

int strcopy(char *dst, const char *src, int max_len);
///\brief copy src to dst (include \0)
///\param dst copy to
///\param src copy from
///\return copy char count (not include \0)
int strcopy(char *dst, const char *src);

///\brief get string length (not include \0)
int strlen(const char *str);

} // namespace util
