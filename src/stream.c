#include "stream.h"

enum { SIZE, EXT, DATA, DATA_END };

static int hexval(char c)
{
	c |= c >= 'A' ? 0x20 : 0;
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

unsigned long chunked_feed(struct chunked *c, char *buf, unsigned long n)
{
	unsigned long i = 0, out = 0;

	while (i < n && !c->done) {
		char ch = buf[i++];

		if (c->state == DATA) {
			buf[out++] = ch;
			if (!--c->left)
				c->state = DATA_END;
		} else if (ch != '\n') {
			if (c->state == SIZE && hexval(ch) >= 0)
				c->left = c->left * 16 + hexval(ch);
			else if (c->state == SIZE && ch == ';')
				c->state = EXT;
		} else if (c->state == DATA_END) {
			c->state = SIZE;
		} else if (c->left) {
			c->state = DATA;
		} else {
			c->done = 1;
		}
	}
	return out;
}

static int starts(const char *p, unsigned long n, const char *lit)
{
	for (unsigned long i = 0; lit[i]; i++)
		if (i >= n || p[i] != lit[i])
			return 0;
	return 1;
}

static unsigned long str(const char *p, unsigned long n, unsigned long j, void (*emit)(char), char *out,
			 unsigned long cap)
{
	unsigned long o = 0;

	while (out && out[o])
		o++;
	while (j < n) {
		char c = p[j++];
		unsigned v = 0;
		int k = 0;

		if (c == '"')
			break;
		if (c == '\\') {
			if (j >= n)
				break;
			switch (c = p[j++]) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': case 'b': case 'f': continue;
			case 'u':
				for (; k < 4 && j < n && hexval(p[j]) >= 0; k++)
					v = v * 16 + hexval(p[j++]);
				if (k < 4 || !v || (v >= 0xdc00 && v <= 0xdfff))
					continue;
				c = v < 0x80 ? (char)v : '?';
				break;
			default: break;
			}
		}
		if (out) {
			if (o + 1 < cap)
				out[o++] = c, out[o] = 0;
		} else if (emit) {
			emit(c);
		}
	}
	return j;
}

static long key(const char *p, unsigned long n, unsigned long i, const char *k)
{
	unsigned long j = i + 1;

	for (; *k; k++, j++)
		if (j >= n || p[j] != *k)
			return -1;
	if (j >= n || p[j++] != '"')
		return -1;
	while (j < n && p[j] == ' ')
		j++;
	if (j >= n || p[j++] != ':')
		return -1;
	while (j < n && p[j] == ' ')
		j++;
	return j < n ? (long)j : -1;
}

static void scan(struct sse *s, const char *p, unsigned long n)
{
	static const char *keys[] = { "content", "tool_calls", "id", "name", "arguments" };
	int depth = 0, tc = 0;

	for (unsigned long i = 0; i < n; i++) {
		struct call *c = s->ncall && s->ncall <= MAX_CALLS ? &s->call[s->ncall - 1] : 0;
		char *o = 0;
		unsigned long cap = 0;
		long j = -1;
		int k = 0;

		if (p[i] == '[' || p[i] == '{')
			depth++;
		else if ((p[i] == ']' || p[i] == '}') && depth-- == tc)
			tc = 0;
		if (p[i] != '"')
			continue;
		while (k < 5 && (j = key(p, n, i, keys[k])) < 0)
			k++;
		if (k == 5) {
			i = str(p, n, i + 1, 0, 0, 0) - 1;
			continue;
		}
		if (k == 1)
			tc = depth + 1;
		if (p[j] != '"') {
			i = j - 1;
			continue;
		}
		if (k == 2 && tc && ++s->ncall <= MAX_CALLS)
			c = &s->call[s->ncall - 1], c->id[0] = c->name[0] = c->args[0] = 0;
		if (tc && c && k == 2)
			o = c->id, cap = sizeof(c->id);
		else if (tc && c && k == 3)
			o = c->name, cap = sizeof(c->name);
		else if (tc && c && k == 4)
			o = c->args, cap = sizeof(c->args);
		i = str(p, n, j + 1, k ? 0 : s->emit, o, cap) - 1;
	}
}

static void sse_line(struct sse *s, const char *p, unsigned long n)
{
	if (n && p[n - 1] == '\r')
		n--;
	if (!starts(p, n, "data:"))
		return;
	p += 5, n -= 5, s->events++;
	if (n && *p == ' ')
		p++, n--;
	if (n == 6 && starts(p, n, "[DONE]"))
		s->done = 1;
	else
		scan(s, p, n);
}

void sse_feed(struct sse *s, const char *p, unsigned long n)
{
	for (unsigned long i = 0; i < n && !s->done; i++) {
		if (p[i] == '\n') {
			if (!s->overflow)
				sse_line(s, s->line, s->len);
			s->len = s->overflow = 0;
		} else if (s->len < sizeof(s->line)) {
			s->line[s->len++] = p[i];
		} else {
			s->overflow = 1;
		}
	}
}

int json_str(const char *p, const char *k, char *out, unsigned long cap)
{
	unsigned long n = 0;
	long j;

	while (p[n])
		n++;
	*out = 0;
	for (unsigned long i = 0; i < n; i++)
		if (p[i] == '"') {
			if ((j = key(p, n, i, k)) >= 0 && p[j] == '"')
				return str(p, n, j + 1, 0, out, cap), 1;
			i = str(p, n, i + 1, 0, 0, 0) - 1;
		}
	return 0;
}
