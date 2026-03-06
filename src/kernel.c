#include "fb.h"
#include "fb_console.h"
#include "vga_console.h"
#include "console.h"
#include "console_backend.h"
#include "gdt.h"
#include "idt.h"
#include "keyboard.h"
#include "isr.h"
#include "dev/dev.h"
#include "vfs/vfs.h"
#include "ext2/ext2.h"
#include "ext4/ext4.h"
#include "block/block.h"
#include "sys/meminfo.h"
#include "sys/boottime.h"
#include "sys/bootlog.h"
#include "sys/process.h"
#include "sys/scheduler.h"
#include "sys/syscall.h"
#include "elf/elf_loader.h"
#include "drivers/e1000.h"
#include "drivers/usb.h"
#include "net/lwip_stack.h"
#include "stdio.h"
#include "string.h"
#include "io_ports.h"

#include <stdint.h>

volatile uint32_t g_timer_ticks;
static const char *g_edge_version = "2.0.8+86_64";
static int g_has_fb_console;

#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_INPUT_HZ 1193182u
#define KERNEL_TIMER_HZ 100u

static void pit_set_rate(uint32_t hz) {
    uint32_t divisor;
    if (hz == 0) hz = KERNEL_TIMER_HZ;
    divisor = PIT_INPUT_HZ / hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xFFFFu) divisor = 0xFFFFu;
    outportb(PIT_CMD_PORT, 0x36); /* ch0, lobyte/hibyte, mode 3 */
    outportb(PIT_CH0_PORT, (uint8_t)(divisor & 0xFFu));
    outportb(PIT_CH0_PORT, (uint8_t)((divisor >> 8) & 0xFFu));
}

static void timer_handler(REGISTERS *r) {
    (void)r;
    g_timer_ticks++;
    lwip_stack_poll();
    usb_poll();
    syscall_tty_irq_poll();
    scheduler_tick();
    if (g_has_fb_console) {
        fb_console_tick(g_timer_ticks);
        fb_console_present();
    }
}

static void ensure_default_system_files(void) {
    static char tmp[256];
    if (vfs_read_file("/etc/os-release", tmp, sizeof(tmp)) < 0) {
        char content[160];
        int n = 0;
        const char *a = "NAME=EdgeOS\n";
        const char *b = "VERSION=";
        const char *c = "\n";
        while (a[n]) { content[n] = a[n]; n++; }
        for (int i = 0; b[i]; ++i) content[n++] = b[i];
        for (int i = 0; g_edge_version[i]; ++i) content[n++] = g_edge_version[i];
        for (int i = 0; c[i]; ++i) content[n++] = c[i];
        content[n] = 0;
        vfs_write_file("/etc/os-release", content, (uint32_t)n);
    }
    if (vfs_read_file("/etc/passwd", tmp, sizeof(tmp)) < 0) {
        const char *pw =
            "root:x:0:0:root:/root:/bin/sh\n"
            "user:x:1000:1000:user:/home/user:/bin/sh\n";
        vfs_write_file("/etc/passwd", pw, (uint32_t)strlen(pw));
    }
    if (vfs_read_file("/etc/group", tmp, sizeof(tmp)) < 0) {
        const char *gr =
            "root:x:0:\n"
            "user:x:1000:user\n";
        vfs_write_file("/etc/group", gr, (uint32_t)strlen(gr));
    }
    if (vfs_read_file("/etc/shadow", tmp, sizeof(tmp)) < 0) {
        const char *sh =
            "root:$6$edgeos$fBGq3tzKqj1d/Mx7YFi1bR0TF0bggz7XwW6PBmucCNFAQA97VvO95xxyFBL4ENOhKcdxohhDO99GCByHvdluA.:0:0:99999:7:::\n"
            "user:$6$edgeos$Bo9eqxKWhKDkW8Uee.aPu4XIwP8kJ0/xeJ5.D325Br2wlNQockexTBvW1/bKqmbY7PVHYvloDdO1SyY8VJbA10:0:0:99999:7:::\n";
        vfs_write_file("/etc/shadow", sh, (uint32_t)strlen(sh));
    }
    (void)vfs_mkdir("/home");
    (void)vfs_mkdir("/home/user");
}

static void ensure_default_dev_entries(void) {
    static const char *chr_nodes[] = {
        "/dev/console", "/dev/tty", "/dev/tty0", "/dev/tty1", "/dev/tty2",
        "/dev/tty3", "/dev/tty4", "/dev/null", "/dev/zero", "/dev/random",
        "/dev/urandom", "/dev/fb0", "/dev/ptmx"
    };
    for (int i = 0; i < (int)(sizeof(chr_nodes) / sizeof(chr_nodes[0])); ++i) {
        (void)vfs_touch(chr_nodes[i]);
    }
    for (int i = 0; i < block_count(); ++i) {
        block_device_t *b = block_get(i);
        char path[32];
        int p = 0;
        if (!b || !b->present) continue;
        path[p++] = '/'; path[p++] = 'd'; path[p++] = 'e'; path[p++] = 'v'; path[p++] = '/';
        for (int j = 0; b->name[j] && p < (int)sizeof(path) - 1; ++j) path[p++] = b->name[j];
        path[p] = 0;
        (void)vfs_touch(path);
    }
}

void kmain(uint32_t magic, void *mb_info) {
    printf("[boot] magic=0x%x mb_info=0x%x\n", magic, (uint32_t)(uintptr_t)mb_info);
    int has_fb = fb_init_from_multiboot2(mb_info) ? 1 : 0;
    g_has_fb_console = has_fb;
    if (has_fb) console_set_backend(&FB_CONSOLE);
    else console_set_backend(&VGA_CONSOLE);
    console_init(COLOR_WHITE, COLOR_BLACK);
    console_clear(COLOR_WHITE, COLOR_BLACK);

    boottime_init();
    bootlog_stage("Initializing GDT");

    gdt_init();
    bootlog_stage("Initializing IDT");
    idt_init();
    bootlog_stage("Initializing keyboard");
    keyboard_init();
    isr_register_interrupt_handler(IRQ_BASE + 0, timer_handler);
    pit_set_rate(KERNEL_TIMER_HZ);
    bootlog_stage("Initializing syscalls");
    syscall_init();
    __asm__ __volatile__("sti");
    bootlog_stage("Initializing memory");
    meminfo_init(magic, mb_info);
    bootlog_stage("Detecting block devices");
    dev_init(magic, mb_info);
    bootlog_stage("Initializing network");
    e1000_init();
    lwip_stack_init();
    bootlog_stage("Initializing USB");
    usb_init();
    bootlog_stage("Initializing VFS");
    vfs_init();

    bootlog_stage("Mounting root filesystem");
    {
        const char *candidates[4];
        int n = 0;
        int mounted = 0;

        if (block_find("ram0")) candidates[n++] = "ram0";
        if (block_find("sda1")) candidates[n++] = "sda1";
        if (block_find("sda")) candidates[n++] = "sda";
        if (block_find("hda")) candidates[n++] = "hda";

        for (int i = 0; i < n && !mounted; ++i) {
            printf("[fs] trying ext4 mount on /dev/%s\n", candidates[i]);
            if (ext4_mount(candidates[i], "/") == 0) {
                mounted = 1;
                break;
            }
            printf("[fs] trying ext2 mount on /dev/%s\n", candidates[i]);
            if (ext2_mount(candidates[i], "/") == 0) mounted = 1;
        }

        if (!mounted) {
            printf("[fs] ext2 mount failed, falling back to mem fs\n");
            vfs_mount("mem", "/", "fat32");
        }
    }
    vfs_mkdir("/etc");
    vfs_mkdir("/root");
    vfs_mkdir("/boot");
    vfs_mkdir("/dev");
    vfs_mkdir("/dev/pts");
    vfs_mkdir("/mnt");
    vfs_mkdir("/lib");
    vfs_mkdir("/proc");
    (void)vfs_mount("proc", "/proc", "proc");
    if (vfs_read_file("/etc/hostname", (char[8]){0}, 1) < 0) {
        (void)vfs_write_file("/etc/hostname", "edgeos\n", 7);
    }
    (void)lwip_stack_reload_system_config();
    /* Do not mutate rootfs before launching init. On LiveCD/ramdisk this can
     * destabilize startup if userspace image is not expecting writes yet. */

    process_init();

    bootlog_stage("INIT: Starting /sbin/init");
    {
        char *init_argv[] = { "init", 0 };
        int init_pid = process_spawn_exec("/sbin/init", 1, init_argv);
        if (init_pid < 0) {
            printf("[init] /sbin/init missing, trying /bin/edgebox\n");
            char *edgebox_argv[] = { "edgebox", 0 };
            init_pid = process_spawn_exec("/bin/edgebox", 1, edgebox_argv);
        }

        if (init_pid < 0) {
            printf("[init] failed to spawn init process\n");
            for (;;) __asm__ __volatile__("hlt");
        }

        for (;;) {
            int status = 0;
            
            int child = process_wait_any(&status);
            
            if (child > 0) {
                printf("[init] reaped pid=%d status=%d\n", child, status);

                if (child == init_pid) {
                    printf("[init] init exited, restarting /sbin/init\n");
                    init_pid = process_spawn_exec("/sbin/init", 1, init_argv);
                    if (init_pid < 0) {
                        printf("[init] restart /sbin/init failed, trying /bin/edgebox\n");
                        char *edgebox_argv[] = { "edgebox", 0 };
                        init_pid = process_spawn_exec("/bin/edgebox", 1, edgebox_argv);
                    }
                }

                continue;
            }
            
            __asm__ __volatile__("sti; hlt");
        }
    }
}
