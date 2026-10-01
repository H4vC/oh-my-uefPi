CA_PEM ?= /etc/ssl/certs/ISRG_Root_X1.pem /etc/ssl/certs/Amazon_Root_CA_1.pem
BEARSSL := vendor/bearssl

EFI  := build/BOOTX64.EFI
IMG  := build/oh-my-uefpi.img
VHDX := build/oh-my-uefpi.vhdx
OBJS := $(patsubst src/%.c,build/%.o,$(wildcard src/*.c))
EFIFLAGS := -ffreestanding -fpic -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -fno-stack-protector \
            -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -Os -U_FORTIFY_SOURCE
CFLAGS := -I/usr/include/efi -I/usr/include/efi/x86_64 -I$(BEARSSL)/inc -Wall -Wextra $(EFIFLAGS) -DEFI_FUNCTION_WRAPPER
# No OS: no /dev/urandom or time(); entropy comes from EFI_RNG_PROTOCOL (and RDRAND), time from GetTime().
BRFLAGS := -I$(BEARSSL)/inc -I$(BEARSSL)/src $(EFIFLAGS) -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 \
           -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0

.PHONY: vhdx serve omarchy-iso cidata clean
vhdx: $(VHDX)

$(BEARSSL)/inc/bearssl.h:
	tools/fetch-bearssl.sh $(BEARSSL)

build/libbearssl.a: $(BEARSSL)/inc/bearssl.h
	@mkdir -p build/bearssl
	ls $(BEARSSL)/src/*/*.c | xargs -P "$$(nproc)" -I{} sh -c \
	   'gcc $(BRFLAGS) -c {} -o build/bearssl/$$(basename {} .c).o'
	rm -f $@ && ar rcs $@ build/bearssl/*.o

build/%.o: src/%.c src/*.h $(BEARSSL)/inc/bearssl.h
	@mkdir -p build
	gcc $(CFLAGS) -c $< -o $@

$(EFI): $(OBJS) build/libbearssl.a
	ld -nostdlib -znocombreloc -shared -Bsymbolic -T /usr/lib/elf_x86_64_efi.lds -L/usr/lib -Lbuild \
	   /usr/lib/crt0-efi-x86_64.o $(OBJS) -o build/oh-my-uefpi.so -lbearssl -lefi -lgnuefi
	objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j '.rel.*' \
	        -j '.rela.*' -j .reloc --target efi-app-x86_64 --subsystem=10 build/oh-my-uefpi.so $@

config.txt:
	cp config.example.txt $@

ca.der:
	for c in $(CA_PEM); do openssl x509 -in $$c -outform der; done > $@

# The ESP: a FAT image with the app, config.txt and ca.der, in a GPT disk as a VHDX for Hyper-V.
$(IMG): $(EFI) config.txt ca.der
	rm -f $@
	mkfs.vfat -C -n OH-MY-UEFPI $@ 33792 >/dev/null
	mmd -i $@ ::/EFI ::/EFI/BOOT
	mcopy -i $@ $(EFI) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ config.txt ::/config.txt
	mcopy -i $@ ca.der ::/ca.der

$(VHDX): $(IMG)
	truncate -s 0 build/gpt.img && truncate -s 36M build/gpt.img
	echo 'start=2048, size=67584, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B' | sfdisk -q -X gpt build/gpt.img
	dd if=$(IMG) of=build/gpt.img bs=1M seek=1 conv=notrunc status=none
	qemu-img convert -f raw -O vhdx -o subformat=dynamic build/gpt.img $@
	rm build/gpt.img

serve: serve/netboot.xyz.efi

serve/netboot.xyz.efi:
	tools/fetch-netbootxyz.sh $@

# Omarchy install (see README): ISO and cidata for tools\omarchy-install.ps1; the disk size matches its -DiskGB.
OMARCHY_ISO := $(HOME)/.cache/uefpi/omarchy-4.0.4.iso
omarchy-iso:
	tools/fetch-omarchy.sh $(OMARCHY_ISO)
	@echo "tools\\omarchy-install.ps1 -Iso '$$(wslpath -w $(OMARCHY_ISO) | tr '\\\\' /)'"

cidata:
	tools/omarchy-cidata.sh build/cidata.iso 42949672960 $(HOME)/.ssh/uefpi_omarchy

clean:
	rm -rf build
