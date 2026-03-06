ASM = nasm
CC  = gcc
LD  = ld
GRUB = grub-mkrescue

SRC    = src
OBJ    = obj
OUT    = out
INC    = include
CONFIG = config
ISO_DIR = $(OUT)/isodir

COMPILE_TIME := $(shell date +"%a %b %d %H:%M:%S %Z %Y")

CFLAGS = -I$(INC) -I$(SRC) \
	-DCOMPILE_TIME="\"$(COMPILE_TIME)\"" \
	-m64 -mno-red-zone -mcmodel=kernel \
	-std=gnu99 -ffreestanding -fno-pie \
	-Wall -Wextra -MMD -MP

LDFLAGS  = -m elf_x86_64 -T $(CONFIG)/linker.ld -nostdlib
ASMFLAGS = -f elf64

C_SRCS  := $(shell find $(SRC) -path '$(SRC)/shell' -prune -o -name '*.c' -print)
ASM_SRCS := $(shell find $(SRC)/asm -name '*.asm')
LWIP_SRCS := \
	$(LWIP_DIR)/src/core/def.c \
	$(LWIP_DIR)/src/core/dns.c \
	$(LWIP_DIR)/src/core/init.c \
	$(LWIP_DIR)/src/core/inet_chksum.c \
	$(LWIP_DIR)/src/core/ip.c \
	$(LWIP_DIR)/src/core/mem.c \
	$(LWIP_DIR)/src/core/memp.c \
	$(LWIP_DIR)/src/core/netif.c \
	$(LWIP_DIR)/src/core/pbuf.c \
	$(LWIP_DIR)/src/core/raw.c \
	$(LWIP_DIR)/src/core/stats.c \
	$(LWIP_DIR)/src/core/sys.c \
	$(LWIP_DIR)/src/core/tcp.c \
	$(LWIP_DIR)/src/core/tcp_in.c \
	$(LWIP_DIR)/src/core/tcp_out.c \
	$(LWIP_DIR)/src/core/timeouts.c \
	$(LWIP_DIR)/src/core/udp.c \
	$(LWIP_DIR)/src/core/ipv4/ip4.c \
	$(LWIP_DIR)/src/core/ipv4/ip4_addr.c \
	$(LWIP_DIR)/src/core/ipv4/icmp.c \
	$(LWIP_DIR)/src/core/ipv4/etharp.c \
	$(LWIP_DIR)/src/core/ipv6/ip6.c \
	$(LWIP_DIR)/src/core/ipv6/ip6_addr.c \
	$(LWIP_DIR)/src/core/ipv6/icmp6.c \
	$(LWIP_DIR)/src/core/ipv6/inet6.c \
	$(LWIP_DIR)/src/core/ipv6/ip6_frag.c \
	$(LWIP_DIR)/src/core/ipv6/nd6.c \
	$(LWIP_DIR)/src/core/ipv6/mld6.c \
	$(LWIP_DIR)/src/core/ipv6/ethip6.c \
	$(LWIP_DIR)/src/netif/ethernet.c

C_OBJS  := $(patsubst $(SRC)/%.c,$(OBJ)/%.o,$(C_SRCS))
ASM_OBJS := $(patsubst $(SRC)/%.asm,$(OBJ)/%.o,$(ASM_SRCS))
LWIP_OBJS := $(patsubst $(LWIP_DIR)/src/%.c,$(OBJ)/lwip/%.o,$(LWIP_SRCS))
OBJS := $(C_OBJS) $(ASM_OBJS) $(LWIP_OBJS)
DEPS := $(OBJS:.o=.d)

TARGET = $(OUT)/edgeos.bin
ISO    = $(OUT)/edgeos.iso
ROOTFS = $(OUT)/rootfs.img
ROOTFS_TOOL = tools/rootfs/mkrootfs.py
ROOTFS_OVERRIDE = tools/rootfs/rootfs_override
ROOTFS_SIZE_MB ?= 256
PORTS_DIR = tools/ports
MUSL_BUILD_SCRIPT = $(PORTS_DIR)/build_musl.sh
BUSYBOX_BUILD_SCRIPT = $(PORTS_DIR)/build_busybox_musl.sh
DROPBEAR_BUILD_SCRIPT = $(PORTS_DIR)/build_dropbear_musl.sh
OPENSSL_BUILD_SCRIPT = $(PORTS_DIR)/build_openssl_musl.sh
PYTHON3_BUILD_SCRIPT = $(PORTS_DIR)/build_python3_musl.sh
TINYX_BUILD_SCRIPT = $(PORTS_DIR)/build_tinyx_musl.sh
MUSL_BUSYBOX_BIN = $(firstword \
	$(wildcard $(USR)/third_party/busybox/busybox_unstripped) \
	$(wildcard $(USR)/third_party/busybox/busybox))
DROPBEAR_BIN = $(firstword \
	$(wildcard $(USR)/third_party/dropbearssh/dropbear) \
	$(wildcard $(USR)/third_party/dropbearssh/src/dropbear))
DROPBEARKEY_BIN = $(firstword \
	$(wildcard $(USR)/third_party/dropbearssh/dropbearkey) \
	$(wildcard $(USR)/third_party/dropbearssh/src/dropbearkey))
OPENSSL_HELPER_BIN ?= $(firstword \
	$(wildcard $(US_BUILD)/openssl) \
	$(wildcard $(USR)/third_party/openssl/apps/openssl) \
	$(wildcard $(USR)/third_party/openssl/openssl))
PYTHON3_BIN ?= $(firstword \
	$(wildcard $(USR)/third_party/python3/python) \
	$(wildcard $(USR)/third_party/python3/python3) \
	$(wildcard $(USR)/third_party/python3/Programs/python) \
	$(wildcard $(USR)/third_party/python3/Programs/python3))
PYTHON3_ROOTFS_OVERRIDE ?= $(USR)/third_party/python3-rootfs-override
PYTHON3_LIB_SRC ?= $(USR)/third_party/python3/Lib
TINYX_DIR ?= $(USR)/third_party/tinyx
TINYX_PREFIX ?= $(OUT)/tinyx-install
TINYX_XFBDEV_BIN ?= $(firstword $(wildcard $(TINYX_PREFIX)/bin/Xfbdev) $(wildcard $(TINYX_DIR)/hw/kdrive/fbdev/Xfbdev))
TINYX_XVFB_BIN ?= $(firstword $(wildcard $(TINYX_PREFIX)/bin/Xvfb))
TINYX_XORG_BIN ?= $(firstword $(wildcard $(TINYX_PREFIX)/bin/Xorg))
TINYX_XEYES_BIN ?= $(firstword $(wildcard $(TINYX_PREFIX)/bin/xeyes))
TINYX_XTERM_BIN ?= $(firstword $(wildcard $(TINYX_PREFIX)/bin/xterm))
TLS_CA_BUNDLE_SRC ?= /etc/ssl/certs/ca-certificates.crt
# Fallback list used when busybox --list cannot run on the build host.
BUSYBOX_APPLETS = add-shell addgroup adduser arch ash awk base32 base64 cat chgrp chmod chown chroot clear cp cut date dd delgroup deluser df diff dos2unix du echo ed egrep env false fgrep free fsync getty grep groups halt head hostname httpd hush id ifconfig iostat kill killall killall5 ln login ls lsof mkdir mv nc netcat nohup nslookup passwd patch pgrep pidof ping ping6 pkill pmap poweroff powertop printenv printf ps pwd reboot remove-shell reset resize rm rmdir sed sh sleep stat su sulogin sync tar tee test top touch tr traceroute traceroute6 true tty uname unix2dos unzip uptime wget whoami whois xargs yes

# -------------------------
# Userspace
# -------------------------

USR = userspace
LWIP_DIR = $(USR)/third_party/lwip
US_BUILD = $(OUT)/userspace
US_CC ?= /opt/musl/bin/musl-gcc
MUSL_RUNTIME_PREFIX ?= $(firstword \
	$(wildcard /tmp/edgeos-musl) \
	/opt/musl)
MUSL_RUNTIME_LIBDIR ?= $(MUSL_RUNTIME_PREFIX)/lib

CFLAGS += -I$(LWIP_DIR)/src/include

CMD_NAMES := $(notdir $(patsubst %/,%,$(dir $(wildcard $(USR)/*/main.c))))
CMD_BINS  := $(addprefix $(US_BUILD)/,$(CMD_NAMES))
APP_SRCS  := $(foreach app,$(CMD_NAMES),$(wildcard $(USR)/$(app)/*.c))
APP_OBJS  := $(patsubst $(USR)/%.c,$(US_BUILD)/.obj/%.o,$(APP_SRCS))

INITBIN = $(US_BUILD)/init

US_CFLAGS = -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -no-pie

US_LDFLAGS = -no-pie \
	-Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
	-L/opt/musl/lib

US_COMPAT_OBJ = $(US_BUILD)/edgeos_compat.o

.PHONY: all clean run usercmds rootfs-install-userland musl busybox-musl dropbear-musl openssl-musl python3-musl tinyx-musl ports

# -------------------------
# Build
# -------------------------

all: $(TARGET) usercmds $(ISO)

$(ROOTFS): $(ROOTFS_TOOL)
	@mkdir -p $(OUT)
	@echo "[rootfs] creating minimal ext4 rootfs image"
	@if command -v python3 >/dev/null 2>&1; then \
		python3 $(ROOTFS_TOOL) --output $(ROOTFS) --size-mb $(ROOTFS_SIZE_MB) --fs ext4 --override-dir $(ROOTFS_OVERRIDE); \
	else \
		echo "[error] python3 is required for rootfs creation"; \
		exit 1; \
	fi

$(TARGET): $(OBJS)
	@mkdir -p $(OUT)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(ISO): $(TARGET) rootfs-install-userland $(CONFIG)/grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp -f $(TARGET) $(ISO_DIR)/boot/edgeos.bin
	cp -f $(CONFIG)/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	cp -f $(ROOTFS) $(ISO_DIR)/boot/rootfs.img
	$(GRUB) -o $(ISO) $(ISO_DIR)

# -------------------------
# Userspace build
# -------------------------

usercmds: $(US_COMPAT_OBJ) $(APP_OBJS) $(CMD_BINS)

$(US_COMPAT_OBJ): $(USR)/edgeos_compat.c $(USR)/edgeos_compat.h
	@mkdir -p $(US_BUILD)
	$(US_CC) $(US_CFLAGS) -c $< -o $@

$(US_BUILD)/.obj/%.o: $(USR)/%.c $(USR)/edgeos_compat.h
	@mkdir -p $(dir $@)
	$(US_CC) $(US_CFLAGS) -include $(USR)/edgeos_compat.h -c $< -o $@

define APP_LINK_RULE
$(US_BUILD)/$(1): $$(filter $(US_BUILD)/.obj/$(1)/%.o,$$(APP_OBJS)) $(US_COMPAT_OBJ)
	@mkdir -p $$(dir $$@)
	$(US_CC) $(US_LDFLAGS) -o $$@ $$^
	@readelf -l $$@ | grep -q '/lib/ld-musl-x86_64.so.1' || (echo "[usercmds] $$@ is not dynamically linked with musl loader"; exit 1)
	@readelf -d $$@ | grep -q 'Shared library: \[libc.so\]' || (echo "[usercmds] $$@ is not linked against musl libc.so"; exit 1)
endef

$(foreach app,$(CMD_NAMES),$(eval $(call APP_LINK_RULE,$(app))))

# -------------------------
# Install to rootfs
# -------------------------

rootfs-install-userland: $(ROOTFS) $(CMD_BINS)
	@echo "[rootfs] installing userland..."
	debugfs -w -R "mkdir /bin" $(ROOTFS) >/dev/null 2>&1 || true
	debugfs -w -R "mkdir /sbin" $(ROOTFS) >/dev/null 2>&1 || true
	debugfs -w -R "mkdir /lib" $(ROOTFS) >/dev/null 2>&1 || true
	debugfs -w -R "rm /sbin/init" $(ROOTFS) >/dev/null 2>&1 || true
	debugfs -w -R "unlink /sbin/init" $(ROOTFS) >/dev/null 2>&1 || true
	debugfs -w -R "write $(INITBIN) /sbin/init" $(ROOTFS) >/dev/null
	@if [ -f "$(US_BUILD)/getty" ]; then \
		debugfs -w -R "rm /sbin/getty" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /sbin/getty" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(US_BUILD)/getty /sbin/getty" $(ROOTFS) >/dev/null; \
	fi
	@for c in $(CMD_NAMES); do \
		debugfs -w -R "rm /bin/$$c" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /bin/$$c" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "rmdir /bin/$$c" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(US_BUILD)/$$c /bin/$$c" $(ROOTFS) >/dev/null; \
	done
	@LD_MUSL_PATH="$(MUSL_RUNTIME_LIBDIR)/ld-musl-x86_64.so.1"; \
	LIBC_SO_PATH="$(MUSL_RUNTIME_LIBDIR)/libc.so"; \
	if [ ! -f "$$LD_MUSL_PATH" ] && [ -f "$$LIBC_SO_PATH" ]; then LD_MUSL_PATH="$$LIBC_SO_PATH"; fi; \
	if [ -n "$$LD_MUSL_PATH" ] && [ "$$LD_MUSL_PATH" != "ld-musl-x86_64.so.1" ] && [ -f "$$LD_MUSL_PATH" ]; then \
		debugfs -w -R "rm /lib/ld-musl-x86_64.so.1" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /lib/ld-musl-x86_64.so.1" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $$LD_MUSL_PATH /lib/ld-musl-x86_64.so.1" $(ROOTFS) >/dev/null; \
	else \
		echo "[rootfs] warning: musl loader not found at $(MUSL_RUNTIME_LIBDIR)"; \
	fi; \
	if [ -n "$$LIBC_SO_PATH" ] && [ "$$LIBC_SO_PATH" != "libc.so" ] && [ -f "$$LIBC_SO_PATH" ]; then \
		debugfs -w -R "rm /lib/libc.so" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /lib/libc.so" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $$LIBC_SO_PATH /lib/libc.so" $(ROOTFS) >/dev/null; \
	else \
		echo "[rootfs] warning: musl libc.so not found at $(MUSL_RUNTIME_LIBDIR)"; \
	fi
	@if [ -f "$(MUSL_BUSYBOX_BIN)" ]; then \
		echo "[rootfs] installing musl busybox as /bin toolset"; \
		debugfs -w -R "rm /bin/busybox" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /bin/busybox" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "rmdir /bin/busybox" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(MUSL_BUSYBOX_BIN) /bin/busybox" $(ROOTFS) >/dev/null; \
		debugfs -w -R "rm /bin/sh" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /bin/sh" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "rmdir /bin/sh" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(MUSL_BUSYBOX_BIN) /bin/sh" $(ROOTFS) >/dev/null; \
		APPLET_LIST=$$($(MUSL_BUSYBOX_BIN) --list 2>/dev/null || true); \
		if [ -z "$$APPLET_LIST" ]; then APPLET_LIST="$(BUSYBOX_APPLETS)"; fi; \
		for a in $$APPLET_LIST; do \
			[ "$$a" = "busybox" ] && continue; \
			debugfs -w -R "rm /bin/$$a" $(ROOTFS) >/dev/null 2>&1 || true; \
			debugfs -w -R "unlink /bin/$$a" $(ROOTFS) >/dev/null 2>&1 || true; \
			debugfs -w -R "rmdir /bin/$$a" $(ROOTFS) >/dev/null 2>&1 || true; \
			debugfs -w -R "write $(MUSL_BUSYBOX_BIN) /bin/$$a" $(ROOTFS) >/dev/null; \
		done; \
	elif [ -f "$(US_BUILD)/esh" ]; then \
		debugfs -w -R "rm /bin/sh" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /bin/sh" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(US_BUILD)/esh /bin/sh" $(ROOTFS) >/dev/null; \
	fi
	@if [ -f "$(DROPBEAR_BIN)" ]; then \
		echo "[rootfs] installing dropbear sshd"; \
		debugfs -w -R "rm /bin/dropbear" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /bin/dropbear" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(DROPBEAR_BIN) /bin/dropbear" $(ROOTFS) >/dev/null; \
	else \
		echo "[rootfs] warning: dropbear binary not found"; \
	fi
	@if [ -f "$(DROPBEARKEY_BIN)" ]; then \
		debugfs -w -R "rm /bin/dropbearkey" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /bin/dropbearkey" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(DROPBEARKEY_BIN) /bin/dropbearkey" $(ROOTFS) >/dev/null; \
	else \
		echo "[rootfs] warning: dropbearkey binary not found"; \
	fi
	@debugfs -w -R "mkdir /etc" $(ROOTFS) >/dev/null 2>&1 || true
	@debugfs -w -R "mkdir /etc/ssl" $(ROOTFS) >/dev/null 2>&1 || true
	@debugfs -w -R "mkdir /etc/ssl/certs" $(ROOTFS) >/dev/null 2>&1 || true
	@if [ -f "$(TLS_CA_BUNDLE_SRC)" ]; then \
		echo "[rootfs] installing CA bundle from $(TLS_CA_BUNDLE_SRC)"; \
		debugfs -w -R "rm /etc/ssl/certs/ca-certificates.crt" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /etc/ssl/certs/ca-certificates.crt" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(TLS_CA_BUNDLE_SRC) /etc/ssl/certs/ca-certificates.crt" $(ROOTFS) >/dev/null; \
		debugfs -w -R "rm /etc/ssl/cert.pem" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /etc/ssl/cert.pem" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(TLS_CA_BUNDLE_SRC) /etc/ssl/cert.pem" $(ROOTFS) >/dev/null; \
	else \
		echo "[rootfs] warning: CA bundle not found at $(TLS_CA_BUNDLE_SRC)"; \
	fi
	@if [ -f "$(OPENSSL_HELPER_BIN)" ]; then \
		echo "[rootfs] installing openssl helper"; \
		debugfs -w -R "rm /bin/openssl" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /bin/openssl" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(OPENSSL_HELPER_BIN) /bin/openssl" $(ROOTFS) >/dev/null; \
	else \
		echo "[rootfs] warning: openssl helper binary not found (set OPENSSL_HELPER_BIN=...)"; \
	fi
	@if [ -f "$(PYTHON3_BIN)" ]; then \
		echo "[rootfs] installing python3"; \
		debugfs -w -R "rm /bin/python3" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /bin/python3" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(PYTHON3_BIN) /bin/python3" $(ROOTFS) >/dev/null; \
	else \
		echo "[rootfs] warning: python3 binary not found (build with make python3-musl)"; \
	fi
	@if [ -f "$(TINYX_XEYES_BIN)" ]; then \
		debugfs -w -R "mkdir /usr/bin" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "rm /usr/bin/xeyes" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /usr/bin/xeyes" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(TINYX_XEYES_BIN) /usr/bin/xeyes" $(ROOTFS) >/dev/null; \
	fi
	@if [ -f "$(TINYX_XTERM_BIN)" ]; then \
		debugfs -w -R "mkdir /usr/bin" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "rm /usr/bin/xterm" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /usr/bin/xterm" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $(TINYX_XTERM_BIN) /usr/bin/xterm" $(ROOTFS) >/dev/null; \
	fi
	@if [ ! -d "$(PYTHON3_ROOTFS_OVERRIDE)" ] && [ -d "$(PYTHON3_LIB_SRC)" ]; then \
		echo "[rootfs] python3 stdlib override missing; staging fallback from $(PYTHON3_LIB_SRC)"; \
		rm -rf "$(PYTHON3_ROOTFS_OVERRIDE)"; \
		mkdir -p "$(PYTHON3_ROOTFS_OVERRIDE)/usr/lib/python3.12"; \
		cp -a "$(PYTHON3_LIB_SRC)"/. "$(PYTHON3_ROOTFS_OVERRIDE)/usr/lib/python3.12/"; \
		find "$(PYTHON3_ROOTFS_OVERRIDE)/usr/lib/python3.12" -type d \( -name test -o -name tests -o -name idlelib -o -name tkinter \) -prune -exec rm -rf {} +; \
		find "$(PYTHON3_ROOTFS_OVERRIDE)/usr/lib/python3.12" -type d -name "__pycache__" -prune -exec rm -rf {} +; \
	fi
	@if [ -d "$(PYTHON3_ROOTFS_OVERRIDE)" ]; then \
		echo "[rootfs] applying python3 stdlib override from $(PYTHON3_ROOTFS_OVERRIDE)"; \
		if [ ! -d "$(PYTHON3_ROOTFS_OVERRIDE)/usr/lib/python3.12/encodings" ]; then \
			echo "[rootfs] warning: python3 stdlib override missing encodings package"; \
		fi; \
		python3 $(ROOTFS_TOOL) --output $(ROOTFS) --override-dir $(PYTHON3_ROOTFS_OVERRIDE) --apply-override-only; \
	fi
	@python3 $(ROOTFS_TOOL) --output $(ROOTFS) --override-dir $(ROOTFS_OVERRIDE) --apply-override-only
	@X_INSTALLED=0; \
	debugfs -w -R "mkdir /usr" $(ROOTFS) >/dev/null 2>&1 || true; \
	debugfs -w -R "mkdir /usr/bin" $(ROOTFS) >/dev/null 2>&1 || true; \
	for xb in Xfbdev Xvfb Xorg Xephyr; do \
		if [ -f "$(TINYX_PREFIX)/bin/$$xb" ]; then \
			echo "[rootfs] installing $$xb"; \
			debugfs -w -R "rm /usr/bin/$$xb" $(ROOTFS) >/dev/null 2>&1 || true; \
			debugfs -w -R "unlink /usr/bin/$$xb" $(ROOTFS) >/dev/null 2>&1 || true; \
			debugfs -w -R "write $(TINYX_PREFIX)/bin/$$xb /usr/bin/$$xb" $(ROOTFS) >/dev/null; \
			X_INSTALLED=1; \
		fi; \
	done; \
	if [ "$$X_INSTALLED" -eq 0 ]; then \
		echo "[rootfs] warning: no X server binary found in $(TINYX_PREFIX)/bin (build with make tinyx-musl)"; \
	fi
	@LD_MUSL_PATH="$(MUSL_RUNTIME_LIBDIR)/ld-musl-x86_64.so.1"; \
	LIBC_SO_PATH="$(MUSL_RUNTIME_LIBDIR)/libc.so"; \
	if [ ! -f "$$LD_MUSL_PATH" ] && [ -f "$$LIBC_SO_PATH" ]; then LD_MUSL_PATH="$$LIBC_SO_PATH"; fi; \
	if [ -n "$$LD_MUSL_PATH" ] && [ "$$LD_MUSL_PATH" != "ld-musl-x86_64.so.1" ] && [ -f "$$LD_MUSL_PATH" ]; then \
		debugfs -w -R "rm /lib/ld-musl-x86_64.so.1" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /lib/ld-musl-x86_64.so.1" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $$LD_MUSL_PATH /lib/ld-musl-x86_64.so.1" $(ROOTFS) >/dev/null; \
	fi; \
	if [ -n "$$LIBC_SO_PATH" ] && [ "$$LIBC_SO_PATH" != "libc.so" ] && [ -f "$$LIBC_SO_PATH" ]; then \
		debugfs -w -R "rm /lib/libc.so" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "unlink /lib/libc.so" $(ROOTFS) >/dev/null 2>&1 || true; \
		debugfs -w -R "write $$LIBC_SO_PATH /lib/libc.so" $(ROOTFS) >/dev/null; \
	fi
	@debugfs -R "stat /sbin/init" $(ROOTFS) >/dev/null

# -------------------------
# Kernel build
# -------------------------

$(OBJ)/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.o: $(SRC)/%.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

$(OBJ)/lwip/%.o: $(LWIP_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: all
	@echo "[run] TIP: if local out/obj are root-owned, use: make OBJ=/tmp/edge_obj OUT=/tmp/edge_out run"
	@NET_MODE=$${EDGEOS_NET:-tap}; \
	TAP_IF=$${EDGEOS_TAP_IF:-tap0}; \
	if [ "$$NET_MODE" = "tap" ]; then \
		echo "[run] using TAP networking (real upstream replies expected): ifname=$$TAP_IF"; \
		NETDEV="-netdev tap,id=net0,ifname=$$TAP_IF,script=no,downscript=no"; \
	else \
		echo "[run] using QEMU user networking (ICMP can be limited on some hosts)"; \
		NETDEV="-netdev user,id=net0"; \
	fi; \
	qemu-system-x86_64 -m 512M \
	-cdrom $(ISO) \
	-device qemu-xhci,id=usb0 \
	-device usb-mouse,bus=usb0.0 \
	-device ich9-ahci,id=ahci \
	-drive file=$(ROOTFS),format=raw,if=none,id=rootfsdisk \
	-device ide-hd,drive=rootfsdisk,bus=ahci.0 \
	$$NETDEV \
	-device e1000,netdev=net0 \
	-rtc base=utc \
	-serial stdio -no-reboot

musl:
	@echo "[ports] building musl toolchain"
	@bash $(MUSL_BUILD_SCRIPT)

busybox-musl: musl
	@echo "[ports] building busybox with musl"
	@bash $(BUSYBOX_BUILD_SCRIPT)

dropbear-musl: musl
	@echo "[ports] building dropbear with musl"
	@bash $(DROPBEAR_BUILD_SCRIPT)

openssl-musl: musl
	@echo "[ports] building openssl with musl"
	@bash $(OPENSSL_BUILD_SCRIPT)

python3-musl: musl
	@echo "[ports] building python3 minimal with musl"
	@bash $(PYTHON3_BUILD_SCRIPT)

tinyx-musl: musl
	@echo "[ports] building X server port with musl"
	@TINYX_DIR="$(abspath $(TINYX_DIR))" \
	TINYX_PREFIX="$(abspath $(TINYX_PREFIX))" \
	bash $(TINYX_BUILD_SCRIPT)

ports: musl busybox-musl dropbear-musl openssl-musl
	@echo "[ports] musl + busybox + dropbear + openssl build complete"

clean:
	rm -rf $(OBJ) $(OUT)

-include $(DEPS)
