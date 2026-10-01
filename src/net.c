#include <efi.h>
#include <efilib.h>
#include <bearssl.h>

#include "efi_net.h"
#include "net.h"
#include "ui.h"

#define CALL uefi_call_wrapper
#define WAIT_MS 30000
#define DHCP_MS 30000
#define DNS_MS 6000
#define CLOSE_MS 1000
#define NS_MAX 8
#define HDR_MAX 16384
#define TX_MAX 16384
#define RX_MAX 65536

static EFI_GUID tcp_sb_guid = TCP4_SB_GUID, tcp_guid = TCP4_GUID, ip4cfg2_guid = IP4CFG2_GUID,
		dns_sb_guid = DNS4_SB_GUID, dns_guid = DNS4_GUID, rng_guid = EFI_RNG_PROTOCOL_GUID;
static EFI_HANDLE nic;
static EFI_SERVICE_BINDING *tcp_sb;
static EFI_IP4_CONFIG2_PROTOCOL *ip4cfg;
static br_x509_trust_anchor *tas;
static UINTN ntas;
static char dns_name[256];
static EFI_IPv4_ADDRESS dns_ip;
static char gw[16];

/* The single open connection: requests are strictly sequential. */
static struct {
	EFI_HANDLE child;
	EFI_TCP4 *tcp;
	EFI_TCP4_CONNECTION_TOKEN cn;
	EFI_TCP4_IO_TOKEN rx, tx;
	EFI_TCP4_CLOSE_TOKEN cl;
	EFI_TCP4_RECEIVE_DATA rxd;
	EFI_TCP4_TRANSMIT_DATA txd;
	volatile BOOLEAN cn_done, rx_done, tx_done, cl_done;
	BOOLEAN open, tls, fin, reported;
	EFI_STATUS err;
	UINT64 ms;
	UINTN hoff, hlen;
} c;
static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static br_sslio_context ioc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static char hbuf[HDR_MAX];

static BOOLEAN prefix(const char *s, const char *p)
{
	while (*p)
		if (*s++ != *p++)
			return FALSE;
	return TRUE;
}

static const char *header(const char *line, const char *name)
{
	for (; *name; line++, name++)
		if ((*line | 0x20) != *name)
			return NULL;
	if (*line++ != ':')
		return NULL;
	while (*line == ' ' || *line == '\t')
		line++;
	return line;
}

static char *fmt_ip(char *p, const UINT8 *a)
{
	for (int i = 0; i < 4; i++)
		p = ui_cpy(ui_fmt_u(p, a[i]), i < 3 ? "." : "");
	return p;
}

static VOID __attribute__((ms_abi)) on_signal(EFI_EVENT e, VOID *flag)
{
	(void)e;
	*(volatile BOOLEAN *)flag = TRUE;
}

static EFI_STATUS event(EFI_EVENT *e, volatile BOOLEAN *flag)
{
	return CALL(BS->CreateEvent, 5, EVT_NOTIFY_SIGNAL, TPL_CALLBACK, on_signal, (VOID *)flag, e);
}

static EFI_STATUS wait_flag(volatile BOOLEAN *f, UINT64 ms)
{
	/* Poll drives the NIC; a millisecond sleep between polls caps throughput at a few frames per ms. */
	for (UINT64 t0 = ui_ms(); !*f; CALL(BS->Stall, 1, 10)) {
		if (ui_ms() - t0 >= ms)
			return EFI_TIMEOUT;
		CALL(c.tcp->Poll, 1, c.tcp);
	}
	return EFI_SUCCESS;
}

EFI_STATUS url_parse(const char *u, struct url *o)
{
	const char *e, *at;
	UINTN n, i;
	UINT32 port = 0;

	o->tls = prefix(u, "https://");
	if (!o->tls && !prefix(u, "http://"))
		return EFI_INVALID_PARAMETER;
	for (e = at = u + (o->tls ? 8 : 7); *e && *e != '/'; e++)
		if (*e == '@')
			at = e + 1;
	n = e - at;
	if (!n || n >= sizeof(o->hostport))
		return EFI_INVALID_PARAMETER;
	CopyMem(o->hostport, (VOID *)at, n);
	o->hostport[n] = 0;
	for (i = 0; o->hostport[i] && o->hostport[i] != ':'; i++)
		o->name[i] = o->hostport[i];
	o->name[i] = 0;
	if (!i)
		return EFI_INVALID_PARAMETER;
	o->port = o->tls ? 443 : 80;
	if (o->hostport[i] == ':') {
		for (const char *d = o->hostport + i + 1; *d; d++) {
			if (*d < '0' || *d > '9' || (port = port * 10 + *d - '0') > 65535)
				return EFI_INVALID_PARAMETER;
		}
		if (!port)
			return EFI_INVALID_PARAMETER;
		o->port = (UINT16)port;
	}
	o->path = *e ? e : "/";
	return EFI_SUCCESS;
}

static UINTN der_len(const UINT8 *p, UINTN size)
{
	UINTN n, hdr, l = 0;

	if (size < 2 || p[0] != 0x30)
		return 0;
	if (p[1] < 0x80) {
		hdr = 2;
		l = p[1];
	} else {
		n = p[1] & 0x7f;
		if (!n || n > 2 || size < 2 + n)
			return 0;
		hdr = 2 + n;
		for (UINTN i = 0; i < n; i++)
			l = l * 256 + p[2 + i];
	}
	return hdr + l <= size ? hdr + l : 0;
}

struct dn {
	UINTN len;
	UINT8 buf[1024];
};

static void dn_add(void *ctx, const void *b, size_t n)
{
	struct dn *d = ctx;

	if (d->len + n <= sizeof(d->buf))
		CopyMem(d->buf + d->len, (VOID *)b, n);
	d->len += n;
}

static void *dup(const void *p, UINTN n)
{
	void *d = AllocatePool(n ? n : 1);

	if (d)
		CopyMem(d, (VOID *)p, n);
	return d;
}

/* ca.der: concatenated DER roots, turned into BearSSL trust anchors. */
static void load_tas(const UINT8 *der, UINTN size)
{
	UINTN count = 0, len;
	char msg[48];

	for (UINTN off = 0; (len = der_len(der + off, size - off)); off += len)
		count++;
	if (!count || !(tas = AllocateZeroPool(count * sizeof(*tas)))) {
		ui_line(UI_ERR, "tls", "ca.der has no DER certificates");
		return;
	}
	for (UINTN off = 0; (len = der_len(der + off, size - off)); off += len) {
		static br_x509_decoder_context dc;
		static struct dn dn;
		br_x509_trust_anchor *ta = &tas[ntas];
		br_x509_pkey *pk;

		dn.len = 0;
		br_x509_decoder_init(&dc, dn_add, &dn);
		br_x509_decoder_push(&dc, der + off, len);
		if (!(pk = br_x509_decoder_get_pkey(&dc)) || dn.len > sizeof(dn.buf))
			continue;
		ta->dn.data = dup(dn.buf, dn.len);
		ta->dn.len = dn.len;
		ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
		ta->pkey.key_type = pk->key_type;
		if (pk->key_type == BR_KEYTYPE_RSA) {
			ta->pkey.key.rsa.n = dup(pk->key.rsa.n, pk->key.rsa.nlen);
			ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
			ta->pkey.key.rsa.e = dup(pk->key.rsa.e, pk->key.rsa.elen);
			ta->pkey.key.rsa.elen = pk->key.rsa.elen;
		} else {
			ta->pkey.key.ec.curve = pk->key.ec.curve;
			ta->pkey.key.ec.q = dup(pk->key.ec.q, pk->key.ec.qlen);
			ta->pkey.key.ec.qlen = pk->key.ec.qlen;
		}
		ntas++;
	}
	ui_cpy(ui_fmt_u(ui_cpy(msg, "ca.der "), ntas), " roots");
	ui_line(ntas ? UI_ACCENT : UI_ERR, "tls", msg);
}

EFI_STATUS net_init(const char *ctx_host, const UINT8 *ca, UINTN ca_len)
{
	EFI_HANDLE *h;
	UINTN n;
	UINT32 dhcp = 1;
	char ip[16], s[300];

	if (!EFI_ERROR(CALL(BS->LocateHandleBuffer, 5, AllHandles, NULL, NULL, &n, &h))) {
		while (n--)
			CALL(BS->ConnectController, 4, h[n], NULL, NULL, TRUE);
		FreePool(h);
	}
	if (EFI_ERROR(CALL(BS->LocateHandleBuffer, 5, ByProtocol, &tcp_sb_guid, NULL, &n, &h))) {
		ui_line(UI_WARN, "net", "no TCP stack");
		return EFI_NOT_FOUND;
	}
	nic = h[0];
	FreePool(h);
	if (EFI_ERROR(CALL(BS->HandleProtocol, 3, nic, &tcp_sb_guid, (VOID **)&tcp_sb)) ||
	    EFI_ERROR(CALL(BS->HandleProtocol, 3, nic, &ip4cfg2_guid, (VOID **)&ip4cfg)))
		return tcp_sb = NULL, EFI_UNSUPPORTED;
	CALL(ip4cfg->SetData, 4, ip4cfg, 1, sizeof(dhcp), &dhcp);
	for (UINT64 t0 = ui_ms();; CALL(BS->Stall, 1, 100000)) {
		EFI_IP4_CONFIG2_INTERFACE_INFO *info;
		UINTN size = 0;
		UINT32 a = 0;

		if (ui_ms() - t0 >= DHCP_MS) {
			ui_line(UI_WARN, "net", "no DHCP lease");
			return tcp_sb = NULL, EFI_TIMEOUT;
		}
		if (CALL(ip4cfg->GetData, 3, ip4cfg, 0, &size, NULL) != EFI_BUFFER_TOO_SMALL || !(info = AllocatePool(size)))
			continue;
		if (!EFI_ERROR(CALL(ip4cfg->GetData, 3, ip4cfg, 0, &size, info))) {
			EFI_IP4_ROUTE_TABLE *rt = info->RouteTable;

			CopyMem(&a, &info->StationAddress, 4);
			for (UINT32 i = 0; rt && i < info->RouteTableSize; i++)
				if (*(UINT32 *)rt[i].GatewayAddress.Addr)
					fmt_ip(gw, rt[i].GatewayAddress.Addr);
		}
		fmt_ip(ip, info->StationAddress.Addr);
		FreePool(info);
		if (a)
			break;
	}
	ui_line(UI_ACCENT, "net", ip);
	ui_cpy(ui_cpy(ui_cpy(s, ctx_host), "|"), ip);
	ui_status_context(s);
	if (ca)
		load_tas(ca, ca_len);
	return EFI_SUCCESS;
}

/* Races the DHCP servers, 1.1.1.1 and 8.8.8.8: Hyper-V's Default Switch DNS never answers the firmware. */
static EFI_STATUS dns_lookup(CHAR16 *name, EFI_IPv4_ADDRESS *ip, EFI_IPv4_ADDRESS *via)
{
	static EFI_IPv4_ADDRESS pub[] = { { { 1, 1, 1, 1 } }, { { 8, 8, 8, 8 } } };
	EFI_IPv4_ADDRESS ns[NS_MAX];
	EFI_SERVICE_BINDING *sb;
	struct {
		EFI_HANDLE child;
		EFI_DNS4_PROTOCOL *dns;
		EFI_DNS4_COMPLETION_TOKEN tok;
		volatile BOOLEAN done;
	} q[NS_MAX] = { 0 };
	EFI_STATUS st = EFI_NOT_FOUND;
	UINTN n, count = 2, win = NS_MAX, live = 1, size = (NS_MAX - 2) * sizeof(*ns);

	CopyMem(ns, pub, sizeof(pub));
	if (!EFI_ERROR(CALL(ip4cfg->GetData, 3, ip4cfg, 4, &size, ns + 2)))
		count = 2 + size / sizeof(*ns);
	if (EFI_ERROR(CALL(BS->HandleProtocol, 3, nic, &dns_sb_guid, (VOID **)&sb)))
		live = 0;
	for (n = 0; live && n < count; n++) {
		EFI_DNS4_CONFIG_DATA cfg = { .DnsServerListCount = 1, .DnsServerList = ns + n, .UseDefaultSetting = TRUE,
					     .Protocol = 17, .RetryCount = 1, .RetryInterval = 2 };

		if (!EFI_ERROR(CALL(sb->CreateChild, 2, sb, &q[n].child)) &&
		    !EFI_ERROR(CALL(BS->HandleProtocol, 3, q[n].child, &dns_guid, (VOID **)&q[n].dns)) &&
		    !EFI_ERROR(CALL(q[n].dns->Configure, 2, q[n].dns, &cfg)) &&
		    !EFI_ERROR(event(&q[n].tok.Event, &q[n].done)) &&
		    EFI_ERROR(CALL(q[n].dns->HostNameToIp, 3, q[n].dns, name, &q[n].tok)))
			q[n].done = TRUE, q[n].tok.Status = EFI_NOT_FOUND;
	}
	for (UINT64 t0 = ui_ms(); live && win == NS_MAX && ui_ms() - t0 < DNS_MS; CALL(BS->Stall, 1, 1000))
		for (n = live = 0; n < NS_MAX && win == NS_MAX; n++)
			if (q[n].tok.Event && !q[n].done)
				live++, CALL(q[n].dns->Poll, 1, q[n].dns);
			else if (q[n].done && !EFI_ERROR(q[n].tok.Status) && q[n].tok.H2AData && q[n].tok.H2AData->IpCount)
				win = n;
	if (win < NS_MAX) {
		*ip = q[win].tok.H2AData->IpList[0];
		*via = ns[win];
		st = EFI_SUCCESS;
	}
	for (n = 0; n < NS_MAX; n++) {
		if (q[n].tok.Event) {
			if (!q[n].done)
				CALL(q[n].dns->Cancel, 2, q[n].dns, &q[n].tok);
			else if (q[n].tok.H2AData)
				FreePool(q[n].tok.H2AData->IpList), FreePool(q[n].tok.H2AData);
			CALL(BS->CloseEvent, 1, q[n].tok.Event);
		}
		if (q[n].dns)
			CALL(q[n].dns->Configure, 2, q[n].dns, NULL);
		if (q[n].child)
			CALL(sb->DestroyChild, 2, sb, q[n].child);
	}
	return st;
}

static BOOLEAN parse_ip(const char *s, EFI_IPv4_ADDRESS *ip)
{
	for (int i = 0; i < 4; i++) {
		UINT32 v = 0;
		const char *d = s;

		for (; *s >= '0' && *s <= '9' && v <= 255; s++)
			v = v * 10 + *s - '0';
		if (s == d || v > 255 || *s != (i < 3 ? '.' : 0))
			return FALSE;
		ip->Addr[i] = (UINT8)v;
		s++;
	}
	return TRUE;
}

const char *net_gateway(void)
{
	return gw;
}

/* NIC path + URI node, as firmware HTTP boot builds it: LoadImage then resolves DeviceHandle to the NIC,
   which iPXE (netboot.xyz) needs to find its network device. Caller frees. */
EFI_DEVICE_PATH *net_boot_path(const char *url)
{
	EFI_DEVICE_PATH *nicp = nic ? DevicePathFromHandle(nic) : NULL, *path;
	UINTN n = strlena((CHAR8 *)url);
	UINT8 *node;

	if (!nicp || !(node = AllocatePool(4 + n)))
		return NULL;
	node[0] = MESSAGING_DEVICE_PATH;
	node[1] = MSG_URI_DP;
	node[2] = (UINT8)(4 + n), node[3] = (UINT8)((4 + n) >> 8);
	CopyMem(node + 4, (VOID *)url, n);
	path = AppendDevicePathNode(nicp, (EFI_DEVICE_PATH *)node);
	FreePool(node);
	return path;
}

EFI_STATUS net_resolve(const char *name, EFI_IPv4_ADDRESS *ip)
{
	EFI_IPv4_ADDRESS via;
	CHAR16 n16[256];
	char msg[300];
	EFI_STATUS st;
	UINTN i;

	if (parse_ip(name, ip))
		return EFI_SUCCESS;
	if (!tcp_sb)
		return EFI_NOT_READY;
	if (dns_name[0] && !strcmpa((CHAR8 *)name, (CHAR8 *)dns_name)) {
		*ip = dns_ip;
		return EFI_SUCCESS;
	}
	for (i = 0; name[i] && i + 1 < sizeof(n16) / sizeof(*n16); i++)
		n16[i] = (UINT8)name[i];
	n16[i] = 0;
	st = dns_lookup(n16, ip, &via);
	if (!EFI_ERROR(st)) {
		ui_cpy(dns_name, name);
		dns_ip = *ip;
		fmt_ip(ui_cpy(fmt_ip(ui_cpy(ui_cpy(msg, name), " "), ip->Addr), " via "), via.Addr);
	} else {
		ui_cpy(ui_cpy(ui_cpy(msg, name), " "), ui_status_name(st));
	}
	ui_line(EFI_ERROR(st) ? UI_ERR : UI_ACCENT, "dns", msg);
	return st;
}

static void tcp_close(void)
{
	EFI_EVENT *ev[] = { &c.cn.CompletionToken.Event, &c.rx.CompletionToken.Event, &c.tx.CompletionToken.Event,
			    &c.cl.CompletionToken.Event };

	if (c.tcp) {
		c.cl.AbortOnClose = TRUE;
		if (c.open && !EFI_ERROR(CALL(c.tcp->Close, 2, c.tcp, &c.cl)))
			wait_flag(&c.cl_done, CLOSE_MS);
		CALL(c.tcp->Configure, 2, c.tcp, NULL);
	}
	if (c.child)
		CALL(tcp_sb->DestroyChild, 2, tcp_sb, c.child);
	for (UINTN i = 0; i < sizeof(ev) / sizeof(*ev); i++)
		if (*ev[i])
			CALL(BS->CloseEvent, 1, *ev[i]);
	ZeroMem(&c, sizeof(c));
}

static EFI_STATUS tcp_open(EFI_IPv4_ADDRESS ip, UINT16 port)
{
	/* Out-of-range values fall back to TcpDxe defaults; the big receive buffer keeps a fast sender's window open. */
	EFI_TCP4_OPTION opt = { .ReceiveBufferSize = 4 << 20, .SendBufferSize = 1 << 20, .EnableWindowScaling = TRUE };
	EFI_TCP4_CONFIG_DATA cfg = { .TimeToLive = 64, .ControlOption = &opt };
	EFI_STATUS st;

	cfg.AccessPoint.UseDefaultAddress = TRUE;
	cfg.AccessPoint.RemoteAddress = ip;
	cfg.AccessPoint.RemotePort = port;
	cfg.AccessPoint.ActiveFlag = TRUE;
	if (EFI_ERROR(st = CALL(tcp_sb->CreateChild, 2, tcp_sb, &c.child)) ||
	    EFI_ERROR(st = CALL(BS->HandleProtocol, 3, c.child, &tcp_guid, (VOID **)&c.tcp)) ||
	    EFI_ERROR(st = CALL(c.tcp->Configure, 2, c.tcp, &cfg)) ||
	    EFI_ERROR(st = event(&c.cn.CompletionToken.Event, &c.cn_done)) ||
	    EFI_ERROR(st = event(&c.rx.CompletionToken.Event, &c.rx_done)) ||
	    EFI_ERROR(st = event(&c.tx.CompletionToken.Event, &c.tx_done)) ||
	    EFI_ERROR(st = event(&c.cl.CompletionToken.Event, &c.cl_done)) ||
	    EFI_ERROR(st = CALL(c.tcp->Connect, 2, c.tcp, &c.cn)) || EFI_ERROR(st = wait_flag(&c.cn_done, WAIT_MS)) ||
	    EFI_ERROR(st = c.cn.CompletionToken.Status))
		return st;
	c.open = TRUE;
	return EFI_SUCCESS;
}

/* > 0 bytes, 0 at FIN, -1 on error (c.err). */
static INTN tcp_recv(UINT8 *buf, UINTN cap)
{
	EFI_STATUS st;

	if (cap > RX_MAX)
		cap = RX_MAX;
	while (!c.fin) {
		c.rxd = (EFI_TCP4_RECEIVE_DATA){ FALSE, cap, 1, { { cap, buf } } };
		c.rx.Packet.RxData = &c.rxd;
		c.rx_done = FALSE;
		if (EFI_ERROR(st = CALL(c.tcp->Receive, 2, c.tcp, &c.rx)) || EFI_ERROR(st = wait_flag(&c.rx_done, c.ms)) ||
		    EFI_ERROR(st = c.rx.CompletionToken.Status)) {
			if (st == EFI_CONNECTION_FIN)
				break;
			c.err = st;
			return -1;
		}
		if (c.rxd.DataLength)
			return c.rxd.DataLength;
	}
	c.fin = TRUE;
	return 0;
}

static EFI_STATUS tcp_send(const UINT8 *buf, UINTN len)
{
	EFI_STATUS st;

	while (len) {
		UINTN n = len < TX_MAX ? len : TX_MAX;

		c.txd = (EFI_TCP4_TRANSMIT_DATA){ TRUE, FALSE, n, 1, { { n, (VOID *)buf } } };
		c.tx.Packet.TxData = &c.txd;
		c.tx_done = FALSE;
		if (EFI_ERROR(st = CALL(c.tcp->Transmit, 2, c.tcp, &c.tx)) || EFI_ERROR(st = wait_flag(&c.tx_done, WAIT_MS)) ||
		    EFI_ERROR(st = c.tx.CompletionToken.Status))
			return c.err = st;
		buf += n, len -= n;
	}
	return EFI_SUCCESS;
}

static int br_rd(void *ctx, unsigned char *buf, size_t len)
{
	INTN n = tcp_recv(buf, len);

	(void)ctx;
	return n > 0 ? (int)n : -1;
}

static int br_wr(void *ctx, const unsigned char *buf, size_t len)
{
	(void)ctx;
	return EFI_ERROR(tcp_send(buf, len)) ? -1 : (int)len;
}

static EFI_STATUS tls_fail(void)
{
	int e = br_ssl_engine_last_error(&sc.eng);
	char msg[96], *p;

	if (e == BR_ERR_OK || e == BR_ERR_IO)
		return c.err ? c.err : EFI_CONNECTION_FIN;
	if (!c.reported) {
		c.reported = TRUE;
		p = ui_fmt_u(ui_cpy(msg, "bearssl error "), e);
		if (e >= BR_ERR_X509_OK && e < BR_ERR_RECV_FATAL_ALERT)
			ui_cpy(p, ": is the server's root in ca.der?");
		else if (e >= BR_ERR_RECV_FATAL_ALERT && e < BR_ERR_SEND_FATAL_ALERT)
			ui_cpy(p, " (alert from server)");
		ui_line(UI_ERR, "tls", msg);
	}
	return EFI_PROTOCOL_ERROR;
}

/* Days since 0000-01-01 (proleptic Gregorian), as BearSSL counts them. */
static UINT32 days(const EFI_TIME *t)
{
	UINT32 y = t->Year - (t->Month <= 2), era = y / 400, yoe = y - era * 400;
	UINT32 doy = (153 * (t->Month > 2 ? t->Month - 3 : t->Month + 9) + 2) / 5 + t->Day - 1;

	return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy + 60;
}

static EFI_STATUS tls_start(const char *name)
{
	EFI_RNG_PROTOCOL *rng;
	UINT8 seed[32];
	EFI_TIME t;

	br_ssl_client_init_full(&sc, &xc, tas, ntas);
	br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
	if (!EFI_ERROR(CALL(BS->LocateProtocol, 3, &rng_guid, NULL, (VOID **)&rng)) &&
	    !EFI_ERROR(CALL(rng->GetRNG, 4, rng, NULL, sizeof(seed), seed)))
		br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));
	if (!EFI_ERROR(CALL(RT->GetTime, 2, &t, NULL)))
		br_x509_minimal_set_time(&xc, days(&t), t.Hour * 3600 + t.Minute * 60 + t.Second);
	if (!br_ssl_client_reset(&sc, name, 0))
		return tls_fail();
	br_sslio_init(&ioc, &sc.eng, br_rd, NULL, br_wr, NULL);
	c.tls = TRUE;
	return EFI_SUCCESS;
}

static EFI_STATUS io_write(const void *p, UINTN n)
{
	if (!c.tls)
		return tcp_send(p, n);
	return br_sslio_write_all(&ioc, p, n) ? tls_fail() : EFI_SUCCESS;
}

static EFI_STATUS io_read(char *buf, UINTN cap, UINTN *n)
{
	INTN r;

	*n = 0;
	if (!c.tls) {
		if ((r = tcp_recv((UINT8 *)buf, cap)) < 0)
			return c.err;
		if (!r)
			return EFI_CONNECTION_FIN;
	} else if ((r = br_sslio_read(&ioc, buf, cap)) < 0) {
		return tls_fail();
	}
	*n = r;
	return EFI_SUCCESS;
}

static EFI_STATUS parse_head(char *p, char *end, struct http_resp *r)
{
	if (!prefix(p, "HTTP/1.") || p[8] != ' ')
		return EFI_PROTOCOL_ERROR;
	for (p += 9; *p >= '0' && *p <= '9'; p++)
		r->code = r->code * 10 + *p - '0';
	while (p < end) {
		char *line = p;
		const char *v;

		while (p < end && *p != '\n')
			p++;
		*p++ = 0;
		if (p - line >= 2 && p[-2] == '\r')
			p[-2] = 0;
		if ((v = header(line, "content-length")))
			for (r->clen = 0; *v >= '0' && *v <= '9'; v++)
				r->clen = r->clen * 10 + *v - '0';
		else if ((v = header(line, "transfer-encoding")))
			r->chunked = prefix(v, "chunked");
		else if ((v = header(line, "location"))) {
			UINTN k = 0;

			while (v[k] && k + 1 < sizeof(r->loc))
				r->loc[k] = v[k], k++;
			r->loc[k] = 0;
		}
	}
	return EFI_SUCCESS;
}

EFI_STATUS http_open(const char *u, const char *method, const char *headers, const char *body, UINTN blen,
		     UINT64 hdr_ms, struct http_resp *r)
{
	struct url p;
	EFI_IPv4_ADDRESS ip;
	EFI_STATUS st;
	UINTN n = 0, got;
	char *h;

	ZeroMem(r, sizeof(*r));
	r->clen = -1;
	if (!tcp_sb)
		return EFI_NOT_READY;
	if (EFI_ERROR(st = url_parse(u, &p)))
		return st;
	if (strlena((CHAR8 *)u) + (headers ? strlena((CHAR8 *)headers) : 0) + 512 > sizeof(hbuf))
		return EFI_BAD_BUFFER_SIZE;
	if (EFI_ERROR(st = net_resolve(p.name, &ip)))
		return st;
	if (EFI_ERROR(st = tcp_open(ip, p.port))) {
		dns_name[0] = 0;
		goto fail;
	}
	c.ms = hdr_ms;
	if (p.tls && EFI_ERROR(st = tls_start(p.name)))
		goto fail;
	h = ui_cpy(ui_cpy(ui_cpy(ui_cpy(hbuf, method), " "), p.path), " HTTP/1.1\r\nHost: ");
	h = ui_cpy(ui_cpy(h, p.hostport), "\r\nUser-Agent: oh-my-uefpi\r\nConnection: close\r\n");
	if (body)
		h = ui_cpy(ui_fmt_u(ui_cpy(h, "Content-Length: "), blen), "\r\n");
	h = ui_cpy(ui_cpy(h, headers ? headers : ""), "\r\n");
	if (EFI_ERROR(st = io_write(hbuf, h - hbuf)) || (body && EFI_ERROR(st = io_write(body, blen))) ||
	    (c.tls && br_sslio_flush(&ioc) && EFI_ERROR(st = tls_fail())))
		goto fail;
	for (;;) {
		if (n == sizeof(hbuf) - 1) {
			st = EFI_BAD_BUFFER_SIZE;
			goto fail;
		}
		if (EFI_ERROR(st = io_read(hbuf + n, sizeof(hbuf) - 1 - n, &got)))
			goto fail;
		for (UINTN i = n < 3 ? 0 : n - 3; i + 3 < n + got; i++)
			if (hbuf[i] == '\r' && hbuf[i + 1] == '\n' && hbuf[i + 2] == '\r' && hbuf[i + 3] == '\n') {
				c.hoff = i + 4;
				c.hlen = n + got;
				hbuf[i + 2] = 0;
				st = parse_head(hbuf, hbuf + i + 2, r);
				if (EFI_ERROR(st))
					goto fail;
				return EFI_SUCCESS;
			}
		n += got;
	}
fail:
	http_close();
	return st;
}

EFI_STATUS http_read(char *buf, UINTN cap, UINTN *n, UINT64 ms)
{
	if (!c.tcp)
		return EFI_NOT_READY;
	if (c.hoff < c.hlen) {
		*n = c.hlen - c.hoff < cap ? c.hlen - c.hoff : cap;
		CopyMem(buf, hbuf + c.hoff, *n);
		c.hoff += *n;
		return EFI_SUCCESS;
	}
	c.ms = ms;
	return io_read(buf, cap, n);
}

void http_close(void)
{
	tcp_close();
}
