
#define _STRING_C
#include <linux/string.h>
#include <linux/module.h>

#undef memmove
void *memmove(void *dest, const void *src, size_t count)
{
	if (dest < src) {
		return memcpy(dest, src, count);
	} else {
		char *p = dest + count;
		const char *s = src + count;
		while (count--)
			*--p = *--s;
	}
	return dest;
}
EXPORT_SYMBOL(memmove);
