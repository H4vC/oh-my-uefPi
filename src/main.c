#include <efi.h>
#include <efilib.h>

#include "llm.h"
#include "ui.h"

static UINTN emitted;

static void emit(char c)
{
	emitted++;
	ui_putc(c);
}

static int starts(const char *s, const char *p)
{
	while (*p)
		if (*s++ != *p++)
			return 0;
	return 1;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
	static char line[2048];
	EFI_STATUS st;
	UINT64 t0;

	InitializeLib(image, systab);
	uefi_call_wrapper(BS->SetWatchdogTimer, 4, 0, 0, 0, NULL);
	ui_init();
	st = llm_init(image);
	if (EFI_ERROR(st))
		ui_line(UI_ERR, "init", ui_status_name(st));
	for (;;) {
		ui_read_line(line, sizeof(line));
		if (!strcmpa((CHAR8 *)line, (CHAR8 *)"/exit"))
			uefi_call_wrapper(RT->ResetSystem, 4, EfiResetShutdown, EFI_SUCCESS, 0, NULL);
		emitted = 0;
		t0 = ui_ms();
		if (starts(line, "/boot ") && line[6]) {
			char *a = line + 6, *sp = a;

			while (*sp && *sp != ' ')
				sp++;
			if (*sp)
				*sp++ = 0;
			st = llm_boot(a, sp);
		} else {
			st = llm_generate(line, emit);
		}
		ui_done(st, ui_ms() - t0, emitted);
	}
}
