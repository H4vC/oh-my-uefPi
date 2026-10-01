#include <stddef.h>

/* BearSSL's libc needs beyond gnu-efi's memcpy/memset. */
void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;

	if (dp < sp)
		while (n--)
			*dp++ = *sp++;
	else
		while (n--)
			dp[n] = sp[n];
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}
