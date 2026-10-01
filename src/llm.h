#pragma once
#include <efi.h>

EFI_STATUS llm_init(EFI_HANDLE image);
EFI_STATUS llm_generate(const char *prompt, void (*emit)(char c));
EFI_STATUS llm_boot(const char *url, const char *args);
