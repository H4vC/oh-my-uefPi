#pragma once

#define MAX_CALLS 4

struct chunked {
	int state, done;
	unsigned long left;
};

struct call {
	char id[64], name[16], args[1024];
};

struct sse {
	void (*emit)(char c);
	int done, overflow, ncall;
	unsigned long events;
	unsigned long len;
	struct call call[MAX_CALLS];
	char line[16384];
};

unsigned long chunked_feed(struct chunked *c, char *buf, unsigned long n);
void sse_feed(struct sse *s, const char *p, unsigned long n);
int json_str(const char *p, const char *key, char *out, unsigned long cap);
