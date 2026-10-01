#pragma once
#include <efi.h>

enum ui_color { UI_TEXT, UI_DIM, UI_ACCENT, UI_OK, UI_ERR, UI_WARN, UI_BRIGHT, UI_BLUE };

void ui_init(void);
UINT64 ui_ms(void);
void ui_putc(char c);
char *ui_fmt_u(char *p, UINT64 v);
char *ui_cpy(char *d, const char *s);
void ui_line(enum ui_color c, const char *label, const char *value);
const char *ui_status_name(EFI_STATUS st);
void ui_status_context(const char *s);
void ui_progress(const char *s);
void ui_read_line(char *buf, UINTN cap);
void ui_done(EFI_STATUS st, UINT64 ms, UINTN chars);
void ui_redraw(void);
