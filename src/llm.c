#include <efi.h>
#include <efilib.h>

#include "efi_net.h"
#include "llm.h"
#include "net.h"
#include "stream.h"
#include "ui.h"

#define CALL uefi_call_wrapper
#define HDR_MS 60000
#define IDLE_MS 20000
#define STALL_MS 90000
#define RX_BUF 65536
#define BODY_MAX 131072
#define ROUND_MAX 16384
#define ROUNDS 4
#define FETCH_MAX 1073741824
#define REDIR_MAX 5

/* Identity plus the served images; their host is the DHCP gateway, i.e. the Hyper-V host running tools/serve.ps1. */
static char sys_prompt[768];
static const char tools_json[] =
	"\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"boot\",\"description\":"
	"\"Boot any UEFI image from any http:// or https:// URL the user asks for: downloads it into memory and runs it "
	"with LoadImage and StartImage, replacing this agent if it does not return. Works for UEFI apps and drivers, "
	"bootloaders, and EFI-stub Linux kernels or UKIs. Returns ok if the image ran and exited, else the error.\","
	"\"parameters\":{\"type\":\"object\",\"properties\":{"
	"\"url\":{\"type\":\"string\",\"description\":\"http(s)://host[:port]/path of the image\"},"
	"\"args\":{\"type\":\"string\",\"description\":\"LoadOptions passed to the image; for Linux this is the kernel command line\"}},"
	"\"required\":[\"url\"]}}}],";

static EFI_HANDLE self;
static char *url, *model = "", *key = "", *thinking = "";
static struct url api;
static struct sse sse;

static BOOLEAN ieq(const char *a, const char *b)
{
	for (; *a && *b && (*a | 0x20) == (*b | 0x20); a++, b++)
		;
	return *a == *b;
}

static char *cpy_json(char *p, const char *s)
{
	for (; *s; s++)
		if (*s == '"' || *s == '\\' || *s == '\n')
			*p++ = '\\', *p++ = *s == '\n' ? 'n' : *s;
		else if ((UINT8)*s >= 0x20)
			*p++ = *s;
	*p = 0;
	return p;
}

static char *load_file(EFI_HANDLE image, CHAR16 *path, UINTN *size)
{
	EFI_LOADED_IMAGE *li;
	EFI_FILE_IO_INTERFACE *fs;
	EFI_FILE_HANDLE root, f;
	EFI_FILE_INFO *info;
	char *buf = NULL;

	if (EFI_ERROR(CALL(BS->HandleProtocol, 3, image, &LoadedImageProtocol, (VOID **)&li)) ||
	    EFI_ERROR(CALL(BS->HandleProtocol, 3, li->DeviceHandle, &FileSystemProtocol, (VOID **)&fs)) ||
	    EFI_ERROR(CALL(fs->OpenVolume, 2, fs, &root)))
		return NULL;
	if (!EFI_ERROR(CALL(root->Open, 5, root, &f, path, EFI_FILE_MODE_READ, 0))) {
		if ((info = LibFileInfo(f))) {
			*size = info->FileSize;
			FreePool(info);
			if ((buf = AllocatePool(*size + 1)) && !EFI_ERROR(CALL(f->Read, 3, f, size, buf)))
				buf[*size] = 0;
		}
		CALL(f->Close, 1, f);
	}
	CALL(root->Close, 1, root);
	return buf;
}

static char *trim(char *s, char *e)
{
	while (*s == ' ')
		s++;
	while (e > s && (e[-1] == ' ' || e[-1] == '\r'))
		e--;
	*e = 0;
	return s;
}

static void parse_config(char *p)
{
	while (*p) {
		char *k = p, *eq = NULL, *v;

		for (; *p && *p != '\n'; p++)
			if (*p == '=' && !eq)
				eq = p;
		if (*p)
			*p++ = 0;
		if (!eq || *k == '#')
			continue;
		v = trim(eq + 1, eq + 1 + strlena((CHAR8 *)eq + 1));
		k = trim(k, eq);
		if (ieq(k, "url"))
			url = v;
		else if (ieq(k, "model"))
			model = v;
		else if (ieq(k, "key"))
			key = v;
		else if (ieq(k, "thinking"))
			thinking = v;
	}
}

static EFI_STATUS setup_url(void)
{
	UINTN n = strlena((CHAR8 *)url);
	char *u = AllocatePool(n + 20);
	EFI_STATUS st;

	if (!u)
		return EFI_OUT_OF_RESOURCES;
	while (n && url[n - 1] == '/')
		n--;
	CopyMem(u, url, n);
	u[n] = 0;
	if (n < 17 || CompareMem(u + n - 17, "/chat/completions", 17))
		ui_cpy(u + n, "/chat/completions");
	if (EFI_ERROR(st = url_parse(u, &api))) {
		FreePool(u);
		return st;
	}
	url = u;
	return EFI_SUCCESS;
}

static void setup_prompt(void)
{
	const char *gw = net_gateway();
	char *p = ui_cpy(sys_prompt, "You are oh-my-uefpi, a UEFI agent: a boot application running with no OS. ");

	if (*gw) {
		p = ui_cpy(ui_cpy(ui_cpy(p, "Omarchy's DXE image is url=http://"), gw), ":8124/omarchy.efi with no args. ");
		p = ui_cpy(ui_cpy(ui_cpy(p, "netboot.xyz's DXE image is url=http://"), gw), ":8124/netboot.xyz.efi with no args. ");
	}
	ui_cpy(p, "Keep replies tiny with no markdown.");
}

EFI_STATUS llm_init(EFI_HANDLE image)
{
	UINTN n, ca_len = 0;
	char *cfg = load_file(image, L"\\config.txt", &n), *ca;
	EFI_IPv4_ADDRESS ip;
	EFI_STATUS st;

	self = image;
	if (cfg)
		parse_config(cfg);
	if (!url || EFI_ERROR(setup_url())) {
		ui_line(UI_WARN, "config", "config.txt needs url=http(s)://host[:port]/path");
		return EFI_INVALID_PARAMETER;
	}
	ui_line(UI_ACCENT, "url", url);
	ui_line(UI_ACCENT, "model", *model ? model : "(server default)");
	ca = load_file(image, L"\\ca.der", &ca_len);
	st = net_init(api.hostport, (UINT8 *)ca, ca_len);
	if (ca)
		FreePool(ca);
	else if (api.tls && !EFI_ERROR(st))
		ui_line(UI_WARN, "tls", "https url but no ca.der; the handshake will fail");
	if (EFI_ERROR(st))
		return st;
	setup_prompt();
	net_resolve(api.name, &ip);
	return EFI_SUCCESS;
}

static EFI_STATUS round(const char *b, UINTN blen, void (*emit)(char))
{
	struct http_resp r;
	struct chunked ch = { 0 };
	char *buf, hdrs[400], *h;
	INT64 got = 0;
	UINTN n, dumped = 0;
	BOOLEAN ok;
	EFI_STATUS st;

	if (!(buf = AllocatePool(RX_BUF)))
		return EFI_OUT_OF_RESOURCES;
	h = ui_cpy(hdrs, "Content-Type: application/json\r\nAccept: text/event-stream\r\n");
	if (*key && strlena((CHAR8 *)key) < sizeof(hdrs) - (h - hdrs) - 32)
		ui_cpy(ui_cpy(ui_cpy(h, "Authorization: Bearer "), key), "\r\n");
	if (EFI_ERROR(st = http_open(url, "POST", hdrs, b, blen, HDR_MS, &r))) {
		FreePool(buf);
		return st;
	}
	if (!(ok = r.code == 200)) {
		ui_fmt_u(hdrs, r.code);
		ui_line(UI_ERR, "http", hdrs);
	}
	sse.emit = emit;
	sse.done = sse.overflow = sse.len = sse.ncall = 0;
	/* DeepSeek holds a stalled request open with ": keep-alive" comments; only data events count as progress. */
	for (UINT64 seen = 0, last = ui_ms();;) {
		if ((st = http_read(buf, RX_BUF, &n, IDLE_MS)) == EFI_CONNECTION_FIN) {
			st = EFI_SUCCESS;
			break;
		}
		if (EFI_ERROR(st))
			break;
		got += n;
		if (r.chunked)
			n = chunked_feed(&ch, buf, n);
		if (ok)
			sse_feed(&sse, buf, n);
		else
			for (UINTN i = 0; i < n && dumped++ < 4096; i++)
				ui_putc(buf[i]);
		if (sse.done || ch.done || (r.clen >= 0 && got >= r.clen))
			break;
		if (sse.events != seen)
			seen = sse.events, last = ui_ms();
		else if (ui_ms() - last >= STALL_MS) {
			ui_line(UI_WARN, "model", "no tokens for 90 s (server sends only keep-alives)");
			st = EFI_TIMEOUT;
			break;
		}
	}
	http_close();
	FreePool(buf);
	if (!ok)
		ui_putc('\n'), st = EFI_ERROR(st) ? st : EFI_HTTP_ERROR;
	return st;
}

static EFI_STATUS fetch(const char *u, char **out, UINTN *out_len)
{
	static char tmp[RX_BUF];
	char cur[1024], *buf = NULL;
	struct http_resp r;
	struct chunked ch = { 0 };
	struct url p;
	UINTN used = 0, cap = 0, n;
	INT64 got = 0;
	EFI_STATUS st;

	if (strlena((CHAR8 *)u) >= sizeof(cur))
		return EFI_INVALID_PARAMETER;
	ui_cpy(cur, u);
	for (int redir = 0;; redir++) {
		if (EFI_ERROR(st = http_open(cur, "GET", "Accept: application/octet-stream\r\n", NULL, 0, HDR_MS, &r)))
			return st;
		if ((r.code == 301 || r.code == 302 || r.code == 303 || r.code == 307 || r.code == 308) &&
		    !EFI_ERROR(url_parse(r.loc, &p))) {
			http_close();
			if (redir == REDIR_MAX)
				return EFI_HTTP_ERROR;
			ui_cpy(cur, r.loc);
			continue;
		}
		if (r.code == 200)
			break;
		http_close();
		ui_fmt_u(tmp, r.code);
		ui_line(UI_ERR, "http", tmp);
		return EFI_HTTP_ERROR;
	}
	if (r.clen > FETCH_MAX || (r.clen > 0 && !r.chunked && !(buf = AllocatePool(cap = r.clen)))) {
		http_close();
		return EFI_OUT_OF_RESOURCES;
	}
	for (UINT64 t0 = ui_ms(), shown = t0;;) {
		if ((st = http_read(tmp, sizeof(tmp), &n, IDLE_MS)) == EFI_CONNECTION_FIN) {
			st = r.chunked || (r.clen >= 0 && got < r.clen) ? EFI_CONNECTION_FIN : EFI_SUCCESS;
			break;
		}
		if (EFI_ERROR(st))
			break;
		got += n;
		if (r.chunked)
			n = chunked_feed(&ch, tmp, n);
		if (used + n > cap) {
			UINTN nc = cap ? cap * 2 : 65536;
			char *nb;

			while (nc < used + n)
				nc *= 2;
			if (nc > FETCH_MAX)
				nc = FETCH_MAX;
			if (used + n > nc || !(nb = AllocatePool(nc))) {
				st = EFI_OUT_OF_RESOURCES;
				break;
			}
			CopyMem(nb, buf, used);
			FreePool(buf);
			buf = nb;
			cap = nc;
		}
		CopyMem(buf + used, tmp, n);
		used += n;
		if (ui_ms() - shown >= 250) {
			char *q = ui_fmt_u(ui_cpy(tmp, "downloading "), used >> 20);

			if (r.clen > 0)
				q = ui_fmt_u(ui_cpy(q, " / "), (UINT64)r.clen >> 20);
			ui_cpy(ui_fmt_u(ui_cpy(q, " MB  "), (used >> 20) * 1000 / (ui_ms() - t0 + 1)), " MB/s");
			ui_progress(tmp);
			shown = ui_ms();
		}
		if (ch.done || (r.clen >= 0 && got >= r.clen)) {
			st = EFI_SUCCESS;
			break;
		}
	}
	http_close();
	if (EFI_ERROR(st)) {
		FreePool(buf);
		return st;
	}
	*out = buf, *out_len = used;
	return EFI_SUCCESS;
}

EFI_STATUS llm_boot(const char *u, const char *args)
{
	EFI_HANDLE child = NULL;
	EFI_LOADED_IMAGE *li;
	EFI_DEVICE_PATH *path;
	CHAR16 *opts = NULL;
	char *img;
	UINTN size, n = 0;
	char msg[64];
	EFI_STATUS st;

	ui_line(UI_ACCENT, "boot", u);
	if (EFI_ERROR(st = fetch(u, &img, &size)))
		return st;
	ui_cpy(ui_fmt_u(ui_cpy(msg, "downloaded "), size), " B");
	ui_line(UI_ACCENT, "boot", msg);
	path = net_boot_path(u);
	st = CALL(BS->LoadImage, 6, FALSE, self, path, img, size, &child);
	if (path)
		FreePool(path);
	if (EFI_ERROR(st)) {
		ui_line(UI_ERR, "boot", ui_status_name(st));
		FreePool(img);
		return st;
	}
	FreePool(img);
	if (args && *args) {
		while (args[n])
			n++;
		opts = AllocatePool((n + 1) * sizeof(CHAR16));
		if (opts) {
			for (UINTN i = 0; i <= n; i++)
				opts[i] = (UINT8)args[i];
		}
	}
	if (!EFI_ERROR(CALL(BS->HandleProtocol, 3, child, &LoadedImageProtocol, (VOID **)&li)) && opts)
		li->LoadOptions = opts, li->LoadOptionsSize = (n + 1) * sizeof(CHAR16);
	CALL(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
	CALL(ST->ConOut->ClearScreen, 1, ST->ConOut);
	st = CALL(BS->StartImage, 3, child, NULL, NULL);
	ui_redraw();
	if (EFI_ERROR(st)) {
		ui_line(UI_ERR, "boot", ui_status_name(st));
		CALL(BS->UnloadImage, 1, child);
	}
	if (opts)
		FreePool(opts);
	return st;
}

EFI_STATUS llm_generate(const char *prompt, void (*emit)(char c))
{
	static char aurl[1024], aargs[512], disp[1600];
	char *body, *p;
	EFI_STATUS st;

	if (!(body = AllocatePool(BODY_MAX)))
		return EFI_OUT_OF_RESOURCES;
	p = ui_cpy(body, "{\"model\":\"");
	if (*model)
		p = cpy_json(p, model);
	p = ui_cpy(p, "\",\"stream\":true,");
	/* DeepSeek thinking mode rejects tool rounds unless reasoning_content is echoed back; "disabled" avoids that. */
	if (*thinking) {
		p = ui_cpy(p, "\"thinking\":{\"type\":\"");
		p = cpy_json(p, thinking);
		p = ui_cpy(p, "\"},");
	}
	p = ui_cpy(p, tools_json);
	p = ui_cpy(p, "\"messages\":[{\"role\":\"system\",\"content\":\"");
	p = ui_cpy(p, sys_prompt);
	p = ui_cpy(p, "\"},{\"role\":\"user\",\"content\":\"");
	p = cpy_json(p, prompt);
	p = ui_cpy(p, "\"}");
	/* p stays at the end of the message list; each round closes it with "]}" past p. */
	for (int r = 0;; r++) {
		ui_cpy(p, "]}");
		if (EFI_ERROR(st = round(body, p - body + 2, emit)))
			break;
		if (sse.ncall > MAX_CALLS)
			sse.ncall = MAX_CALLS;
		if (!sse.ncall || r == ROUNDS || (UINTN)(p - body) + ROUND_MAX > BODY_MAX)
			break;
		p = ui_cpy(p, ",{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[");
		for (int i = 0; i < sse.ncall; i++) {
			struct call *c = &sse.call[i];

			p = ui_cpy(p, i ? ",{\"id\":\"" : "{\"id\":\"");
			p = cpy_json(p, c->id);
			p = ui_cpy(p, "\",\"type\":\"function\",\"function\":{\"name\":\"");
			p = cpy_json(p, c->name);
			p = ui_cpy(p, "\",\"arguments\":\"");
			p = cpy_json(p, c->args);
			p = ui_cpy(p, "\"}}");
		}
		p = ui_cpy(p, "]}");
		for (int i = 0; i < sse.ncall; i++) {
			struct call *c = &sse.call[i];
			EFI_STATUS ts = EFI_NOT_FOUND;

			json_str(c->args, "url", aurl, sizeof(aurl));
			json_str(c->args, "args", aargs, sizeof(aargs));
			ui_cpy(ui_cpy(ui_cpy(ui_cpy(ui_cpy(disp, c->name), " "), aurl), " "), aargs);
			ui_line(UI_ACCENT, "tool", disp);
			if (!strcmpa((CHAR8 *)c->name, (CHAR8 *)"boot"))
				ts = llm_boot(aurl, aargs);
			p = ui_cpy(p, ",{\"role\":\"tool\",\"tool_call_id\":\"");
			p = cpy_json(p, c->id);
			p = ui_cpy(p, "\",\"content\":\"");
			p = ui_cpy(p, EFI_ERROR(ts) ? ui_status_name(ts) : "ok");
			p = ui_cpy(p, "\"}");
		}
	}
	FreePool(body);
	return st;
}
