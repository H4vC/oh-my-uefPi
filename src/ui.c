#include <efi.h>
#include <efilib.h>

#include "efi_net.h"
#include "ui.h"

#undef OUT
#define OUT ST->ConOut

static const UINTN fg[] = { EFI_LIGHTGRAY, EFI_DARKGRAY, EFI_LIGHTCYAN, EFI_LIGHTGREEN,
			    EFI_LIGHTRED,  EFI_YELLOW,   EFI_WHITE,     EFI_LIGHTBLUE };
static const CHAR16 *g_pi, *g_prompt, *g_sep, *g_rule, *g_dot, *g_ok, *g_bad, *g_edge;
static UINT64 tpm = 1;
static UINTN W, H, TR, row, col, attr_now = (UINTN)-1, bx;
static CHAR16 *cell, bc[161];
static UINT8 *cattr, ba[161];
static BOOLEAN at_cursor;
static enum ui_color color, st_color;
static const CHAR16 *st_mark;
static char ctx[128], st[64];
static int tail;

static UINT64 rdtsc(void)
{
	UINT32 lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return (UINT64)hi << 32 | lo;
}

UINT64 ui_ms(void)
{
	return rdtsc() / tpm;
}

char *ui_cpy(char *d, const char *s)
{
	while ((*d = *s++))
		d++;
	return d;
}

char *ui_fmt_u(char *p, UINT64 v)
{
	char t[20];
	int n = 0;

	do
		t[n++] = '0' + v % 10;
	while (v /= 10);
	while (n)
		*p++ = t[--n];
	*p = 0;
	return p;
}

static void at(UINTN x, UINTN y)
{
	uefi_call_wrapper(OUT->SetCursorPosition, 3, OUT, x, y);
	at_cursor = FALSE;
}

static void text(UINT8 a, CHAR16 *s)
{
	if (attr_now != fg[a])
		uefi_call_wrapper(OUT->SetAttribute, 2, OUT, attr_now = fg[a]);
	uefi_call_wrapper(OUT->OutputString, 2, OUT, s);
}

static void paint(UINTN y, const CHAR16 *c, const UINT8 *a)
{
	CHAR16 run[161];

	at(0, y);
	for (UINTN x = 0, k; x < W - 1; x = k) {
		for (k = x; k < W - 1 && a[k] == a[x]; k++)
			run[k - x] = c[k];
		run[k - x] = 0;
		text(a[x], run);
	}
}

static void bw(enum ui_color c, const CHAR16 *s)
{
	for (; *s && bx < W - 1; bx++)
		bc[bx] = *s++, ba[bx] = c;
}

static void bw8(enum ui_color c, const char *s)
{
	for (; *s && bx < W - 1; bx++)
		bc[bx] = (UINT8)*s++, ba[bx] = c;
}

static void bar(UINTN y)
{
	while (bx < W - 1)
		bc[bx] = L' ', ba[bx++] = UI_TEXT;
	paint(y, bc, ba);
	bx = 0;
}

static void draw_status(void)
{
	bw8(UI_TEXT, " ");
	bw(UI_ACCENT, g_pi);
	bw8(UI_BRIGHT, " oh-my-uefpi");
	for (char *p = ctx; *p; p++) {
		char s[2] = { *p };

		if (p == ctx || *p == '|')
			bw(UI_DIM, g_sep);
		if (*p != '|')
			bw8(UI_TEXT, s);
	}
	bw(UI_DIM, g_sep);
	if (st_mark)
		bw(st_color, st_mark), bw8(st_color, " ");
	bw8(st_color, st);
	bar(H - 1);
}

static void status(enum ui_color c, const CHAR16 *mark, const char *s)
{
	st[sizeof(st) - 1] = 0;
	for (UINTN i = 0; i < sizeof(st) - 1 && (st[i] = s[i]); i++)
		;
	st_color = c;
	st_mark = mark;
	draw_status();
}

void ui_progress(const char *s)
{
	status(UI_ACCENT, NULL, s);
}

void ui_status_context(const char *s)
{
	ctx[sizeof(ctx) - 1] = 0;
	for (UINTN i = 0; i < sizeof(ctx) - 1 && (ctx[i] = s[i]); i++)
		;
	draw_status();
}

static void draw_input(const char *buf, UINTN n)
{
	UINTN room, cx;

	bw(UI_ACCENT, g_edge);
	bw8(UI_TEXT, " ");
	bw(UI_BLUE, g_prompt);
	bw8(UI_TEXT, " ");
	room = W - 2 - bx;
	for (UINTN i = n > room ? n - room : 0; i < n; i++, bx++)
		bc[bx] = (UINT8)buf[i], ba[bx] = UI_BRIGHT;
	cx = bx;
	bar(H - 3);
	at(cx, H - 3);
}

static void newline(void)
{
	col = 0;
	at_cursor = FALSE;
	if (row + 1 < TR) {
		row++;
		return;
	}
	CopyMem(cell, cell + W, (TR - 1) * W * sizeof(CHAR16));
	CopyMem(cattr, cattr + W, (TR - 1) * W);
	for (UINTN x = 0; x < W; x++)
		cell[(TR - 1) * W + x] = L' ', cattr[(TR - 1) * W + x] = UI_TEXT;
	for (UINTN y = 0; y < TR; y++)
		paint(y, cell + y * W, cattr + y * W);
}

static void put16(CHAR16 c)
{
	CHAR16 *r = cell + row * W, w[80], s[2] = { c, 0 };
	UINT8 *a = cattr + row * W, wa[80];
	UINTN b = col, n = 0;

	if (col >= W - 1) {
		while (b && r[b - 1] != L' ')
			b--;
		if (c != L' ' && b && col - b < W / 2)
			for (n = col - b; b < col; b++)
				w[b + n - col] = r[b], wa[b + n - col] = a[b], r[b] = L' ';
		if (n)
			paint(row, r, a);
		newline();
		r = cell + row * W, a = cattr + row * W;
		CopyMem(r, w, n * sizeof(CHAR16));
		CopyMem(a, wa, n);
		if (n)
			paint(row, r, a), col = n;
		if (c == L' ')
			return;
	}
	r[col] = c;
	a[col++] = color;
	if (!at_cursor)
		at(col - 1, row), at_cursor = TRUE;
	text(color, s);
}

static void puts16(const CHAR16 *s)
{
	while (*s)
		put16(*s++);
}

void ui_putc(char ch)
{
	UINT8 c = ch;

	if (c >= 0x80) {
		if (c >= 0xc0)
			tail = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
		else if (tail && tail--)
			return;
		put16(L'?');
	} else if ((tail = 0, c == '\n')) {
		newline();
	} else if (c == '\t' || (c >= 0x20 && c < 0x7f)) {
		put16(c == '\t' ? L' ' : c);
	}
}

static void ui_puts(const char *s)
{
	while (*s)
		ui_putc(*s++);
}

void ui_line(enum ui_color c, const char *label, const char *value)
{
	UINTN n = 0;

	if (col)
		newline();
	ui_puts("  ");
	color = c;
	puts16(c == UI_ERR ? g_bad : g_dot);
	color = c == UI_ACCENT ? UI_DIM : c;
	ui_puts(" ");
	for (; label[n]; n++)
		ui_putc(label[n]);
	for (; n < 8; n++)
		ui_putc(' ');
	color = c == UI_ACCENT ? UI_TEXT : c;
	ui_puts(value);
	newline();
	color = UI_TEXT;
}

const char *ui_status_name(EFI_STATUS s)
{
	switch (s) {
	case EFI_TIMEOUT: return "timeout";
	case EFI_NOT_READY: return "not ready";
	case EFI_NOT_FOUND: return "not found";
	case EFI_NO_MEDIA: return "no media";
	case EFI_NO_MAPPING: return "no mapping";
	case EFI_INVALID_PARAMETER: return "bad param";
	case EFI_UNSUPPORTED: return "unsupported";
	case EFI_OUT_OF_RESOURCES: return "no memory";
	case EFI_ACCESS_DENIED: return "access denied";
	case EFI_ABORTED: return "aborted";
	case EFI_BAD_BUFFER_SIZE: return "too large";
	case EFI_PROTOCOL_ERROR: return "protocol error";
	case EFI_HTTP_ERROR: return "http error";
	case EFI_CONNECTION_FIN: return "connection closed";
	case EFI_CONNECTION_RESET: return "connection reset";
	case EFI_CONNECTION_REFUSED: return "connection refused";
	default: return "error";
	}
}

static void rules(void)
{
	for (UINTN y = H - 4; y <= H - 2; y += 2) {
		while (bx < W - 1)
			bw(UI_DIM, g_rule);
		bar(y);
	}
}

void ui_redraw(void)
{
	uefi_call_wrapper(OUT->SetMode, 2, OUT, 0);
	uefi_call_wrapper(OUT->SetAttribute, 2, OUT, attr_now = fg[UI_TEXT]);
	uefi_call_wrapper(OUT->ClearScreen, 1, OUT);
	uefi_call_wrapper(OUT->EnableCursor, 2, OUT, TRUE);
	for (UINTN y = 0; y < TR; y++)
		paint(y, cell + y * W, cattr + y * W);
	rules();
	draw_status();
	at_cursor = FALSE;
}

static const CHAR16 *pick(const CHAR16 *fancy, const CHAR16 *plain)
{
	return EFI_ERROR(uefi_call_wrapper(OUT->TestString, 2, OUT, (CHAR16 *)fancy)) ? plain : fancy;
}

void ui_init(void)
{
	static const CHAR16 *logo[] = { L"  \x2550\x2550\x2550\x2566\x2550\x2550\x2550\x2566\x2550\x2550\x2550   ",
					L"     \x2551   \x2551      ", L"     \x2551   \x255a\x2550\x2550   " };
	static const char *title[] = { "oh-my-uefpi", "boot-to-prompt, no os", "" };
	UINT64 t0 = rdtsc();
	BOOLEAN boxes;

	uefi_call_wrapper(BS->Stall, 1, 20000);
	tpm = (rdtsc() - t0) / 20 ?: 1;
	uefi_call_wrapper(OUT->SetMode, 2, OUT, 0);
	if (EFI_ERROR(uefi_call_wrapper(OUT->QueryMode, 4, OUT, 0, &W, &H)) || W < 40 || W > 160 || H < 10)
		W = 80, H = 25;
	TR = H - 4;
	cell = AllocatePool(TR * W * sizeof(CHAR16));
	cattr = AllocateZeroPool(TR * W);
	for (UINTN i = 0; i < TR * W; i++)
		cell[i] = L' ';
	g_pi = pick(L"\x03c0", L"pi");
	g_prompt = L">";
	g_sep = pick(L" \x00b7 ", L" | ");
	g_rule = pick(L"\x2500", L"-");
	g_dot = pick(L"\x2022", L"*");
	g_ok = pick(L"\x2713", L"ok");
	g_bad = pick(L"\x2717", L"x");
	g_edge = pick(L"\x258c", pick(L"\x2502", L"|"));
	boxes = pick(logo[0], NULL) && pick(logo[2], NULL);

	uefi_call_wrapper(OUT->ClearScreen, 1, OUT);
	uefi_call_wrapper(OUT->EnableCursor, 2, OUT, TRUE);
	rules();
	status(UI_DIM, NULL, "starting");
	newline();
	for (int i = 0; i < 3; i++) {
		color = UI_ACCENT;
		if (boxes)
			puts16(logo[i]);
		color = i ? UI_DIM : UI_BRIGHT;
		ui_puts(title[i]);
		newline();
	}
	color = UI_TEXT;
}

void ui_read_line(char *buf, UINTN cap)
{
	static BOOLEAN ready;
	UINTN n = 0;

	if (!ready) {
		ready = TRUE;
		newline();
		status(UI_DIM, NULL, "ready  (/exit or ctrl-d powers off)");
	}
	draw_input(buf, 0);
	for (;;) {
		EFI_INPUT_KEY key;
		UINTN idx;
		CHAR16 c;

		uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &idx);
		if (EFI_ERROR(uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key)))
			continue;
		c = key.UnicodeChar;
		if (c == 4) {
			ui_cpy(buf, "/exit");
			return;
		}
		if ((c == CHAR_CARRIAGE_RETURN || c == CHAR_LINEFEED) && n)
			break;
		if (c == CHAR_BACKSPACE || c == 0x7f)
			n -= n > 0;
		else if (key.ScanCode == SCAN_ESC)
			n = 0;
		else if (c >= 0x20 && c < 0x7f && n + 1 < cap)
			buf[n++] = c;
		else
			continue;
		draw_input(buf, n);
	}
	buf[n] = 0;
	draw_input(buf, 0);
	if (col)
		newline();
	color = UI_BLUE;
	puts16(g_prompt);
	ui_puts(" ");
	color = UI_BRIGHT;
	ui_puts(buf);
	newline();
	color = UI_TEXT;
	status(UI_ACCENT, NULL, "working...");
}

void ui_done(EFI_STATUS s, UINT64 ms, UINTN chars)
{
	char t[16], b[96];

	ui_cpy(ui_fmt_u(ui_cpy(ui_fmt_u(t, ms / 1000), "."), ms % 1000 / 100), "s");
	if (col)
		newline();
	if (EFI_ERROR(s)) {
		ui_cpy(ui_cpy(ui_cpy(b, ui_status_name(s)), "  "), t);
		ui_line(UI_ERR, "error", b);
		status(UI_ERR, g_bad, ui_status_name(s));
	} else {
		ui_cpy(ui_fmt_u(ui_cpy(ui_cpy(b, t), "  "), chars), " chars");
		status(UI_OK, g_ok, b);
	}
	newline();
}
