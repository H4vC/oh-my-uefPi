#pragma once
#include <efi.h>

struct url {
	char hostport[256], name[256];
	const char *path;
	UINT16 port;
	BOOLEAN tls;
};

struct http_resp {
	UINT32 code;
	INT64 clen;
	BOOLEAN chunked;
	char loc[512];
};

EFI_STATUS url_parse(const char *u, struct url *o);
EFI_STATUS net_init(const char *ctx_host, const UINT8 *ca, UINTN ca_len);
const char *net_gateway(void);
EFI_DEVICE_PATH *net_boot_path(const char *url);
EFI_STATUS net_resolve(const char *name, EFI_IPv4_ADDRESS *ip);
EFI_STATUS http_open(const char *url, const char *method, const char *headers, const char *body, UINTN blen,
		     UINT64 hdr_ms, struct http_resp *r);
EFI_STATUS http_read(char *buf, UINTN cap, UINTN *n, UINT64 ms);
void http_close(void);
