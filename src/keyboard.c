#include "keyboard.h"
#include "console.h"
#include "idt.h"
#include "io_ports.h"
#include "isr.h"
#include "sys/boottime.h"
#include "types.h"
#include "string.h"

static BOOL g_caps_lock = FALSE;
static BOOL g_shift_pressed = FALSE;
static BOOL g_ctrl_pressed = FALSE;
volatile char g_ch = 0, g_scan_code = 0;
static int g_extended = 0;
static volatile int g_skip_irq_scancode = 0;
static volatile uint64_t g_keyboard_irq_count;
static volatile uint64_t g_keyboard_last_tsc;
static volatile uint32_t g_sigint_pending;

#define KBD_BUF_SIZE 256
static char kbd_buf[KBD_BUF_SIZE];
static volatile int kbd_head;
static volatile int kbd_tail;

#define MOUSE_BUF_SIZE 1024
static uint8_t mouse_buf[MOUSE_BUF_SIZE];
static volatile int mouse_head;
static volatile int mouse_tail;
#define MOUSE_EVENT_BUF_SIZE 8192
static uint8_t mouse_event_buf[MOUSE_EVENT_BUF_SIZE];
static volatile int mouse_event_head;
static volatile int mouse_event_tail;
static uint8_t g_mouse_buttons;
static int g_mouse_wheel_mode;
static int g_mouse_ext_prefix;

#define LINUX_EV_SYN 0x00u
#define LINUX_EV_KEY 0x01u
#define LINUX_EV_REL 0x02u
#define LINUX_SYN_REPORT 0u
#define LINUX_REL_X 0u
#define LINUX_REL_Y 1u
#define LINUX_REL_WHEEL 8u
#define LINUX_BTN_LEFT 0x110u
#define LINUX_BTN_RIGHT 0x111u
#define LINUX_BTN_MIDDLE 0x112u

typedef struct __attribute__((packed)) {
    int64_t tv_sec;
    int64_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} edge_linux_input_event_t;

static void kbd_buf_push_char(char ch) {
    int next_head = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next_head == kbd_tail) return;
    kbd_buf[kbd_head] = ch;
    kbd_head = next_head;
}

static void mouse_buf_push_byte(uint8_t b) {
    int next_head = (mouse_head + 1) % MOUSE_BUF_SIZE;
    if (next_head == mouse_tail) return;
    mouse_buf[mouse_head] = b;
    mouse_head = next_head;
}

static void mouse_event_buf_push_byte(uint8_t b) {
    int next_head = (mouse_event_head + 1) % MOUSE_EVENT_BUF_SIZE;
    if (next_head == mouse_event_tail) return;
    mouse_event_buf[mouse_event_head] = b;
    mouse_event_head = next_head;
}

static void mouse_event_push(uint16_t type, uint16_t code, int32_t value) {
    edge_linux_input_event_t ev;
    uint64_t us = boottime_monotonic_us();
    ev.tv_sec = (int64_t)(us / 1000000ull);
    ev.tv_usec = (int64_t)(us % 1000000ull);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    for (uint32_t i = 0; i < sizeof(ev); ++i) {
        mouse_event_buf_push_byte(((const uint8_t *)&ev)[i]);
    }
}

static void mouse_emit_linux_events(int dx, int dy, int wheel, uint8_t old_buttons, uint8_t new_buttons, int wheel_present) {
    if (dx != 0) mouse_event_push(LINUX_EV_REL, LINUX_REL_X, dx);
    if (dy != 0) mouse_event_push(LINUX_EV_REL, LINUX_REL_Y, dy);
    if (wheel_present && wheel != 0) mouse_event_push(LINUX_EV_REL, LINUX_REL_WHEEL, wheel);
    if ((old_buttons & 0x01u) != (new_buttons & 0x01u)) {
        mouse_event_push(LINUX_EV_KEY, LINUX_BTN_LEFT, (new_buttons & 0x01u) ? 1 : 0);
    }
    if ((old_buttons & 0x02u) != (new_buttons & 0x02u)) {
        mouse_event_push(LINUX_EV_KEY, LINUX_BTN_RIGHT, (new_buttons & 0x02u) ? 1 : 0);
    }
    if ((old_buttons & 0x04u) != (new_buttons & 0x04u)) {
        mouse_event_push(LINUX_EV_KEY, LINUX_BTN_MIDDLE, (new_buttons & 0x04u) ? 1 : 0);
    }
    if (dx != 0 || dy != 0 || (wheel_present && wheel != 0) || old_buttons != new_buttons) {
        mouse_event_push(LINUX_EV_SYN, LINUX_SYN_REPORT, 0);
    }
}

static void mouse_emit_ps2_packet(int dx, int dy, int wheel, uint8_t buttons, int wheel_present) {
    uint8_t b0 = 0x08u | (buttons & 0x07u);
    uint8_t b1, b2;
    if (dx > 127) dx = 127;
    if (dx < -127) dx = -127;
    if (dy > 127) dy = 127;
    if (dy < -127) dy = -127;
    if (dx < 0) b0 |= 0x10u;
    if (dy < 0) b0 |= 0x20u;
    b1 = (uint8_t)((int8_t)dx);
    b2 = (uint8_t)((int8_t)dy);
    mouse_buf_push_byte(b0);
    mouse_buf_push_byte(b1);
    mouse_buf_push_byte(b2);
    if (wheel_present) g_mouse_wheel_mode = 1;
    if (g_mouse_wheel_mode) {
        if (wheel > 127) wheel = 127;
        if (wheel < -127) wheel = -127;
        mouse_buf_push_byte((uint8_t)((int8_t)wheel));
    }
}

void keyboard_mouse_emit_packet(int dx, int dy, uint8_t buttons) {
    keyboard_mouse_emit_packet_ex(dx, dy, 0, buttons, 0);
}

void keyboard_mouse_emit_packet_ex(int dx, int dy, int wheel, uint8_t buttons, int wheel_present) {
    uint8_t old_buttons = g_mouse_buttons;
    uint8_t next_buttons = (uint8_t)(buttons & 0x07u);
    g_mouse_buttons = next_buttons;
    mouse_emit_ps2_packet(dx, dy, wheel, next_buttons, wheel_present);
    mouse_emit_linux_events(dx, dy, wheel, old_buttons, next_buttons, wheel_present);
}

static void mouse_emulate_from_scancode(int scancode) {
    int release = 0;
    if (scancode == 0xE0) {
        g_mouse_ext_prefix = 1;
        return;
    }
    if (scancode & 0x80) {
        release = 1;
        scancode &= 0x7F;
    }

    if (g_mouse_ext_prefix) {
        if (!release) {
            if (scancode == SCAN_CODE_KEY_UP) keyboard_mouse_emit_packet_ex(0, +8, 0, g_mouse_buttons, 0);
            else if (scancode == SCAN_CODE_KEY_DOWN) keyboard_mouse_emit_packet_ex(0, -8, 0, g_mouse_buttons, 0);
            else if (scancode == SCAN_CODE_KEY_LEFT) keyboard_mouse_emit_packet_ex(-8, 0, 0, g_mouse_buttons, 0);
            else if (scancode == SCAN_CODE_KEY_RIGHT) keyboard_mouse_emit_packet_ex(+8, 0, 0, g_mouse_buttons, 0);
        }
        g_mouse_ext_prefix = 0;
        return;
    }

    /* Keyboard-emulated mouse buttons: z=left, x=right, c=middle */
    if (scancode == SCAN_CODE_KEY_Z || scancode == SCAN_CODE_KEY_X || scancode == SCAN_CODE_KEY_C) {
        uint8_t mask = (scancode == SCAN_CODE_KEY_Z) ? 0x1u :
                       (scancode == SCAN_CODE_KEY_X) ? 0x2u : 0x4u;
        uint8_t next = release ? (uint8_t)(g_mouse_buttons & ~mask) : (uint8_t)(g_mouse_buttons | mask);
        if (next != g_mouse_buttons) {
            keyboard_mouse_emit_packet_ex(0, 0, 0, next, 0);
        }
    }
}

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// see scan codes defined in keyboard.h for index
char g_scan_code_chars[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0
};

static int get_scancode() {
    return inportb(KEYBOARD_DATA_PORT);
}
char alternate_chars(char ch);

static int decode_scancode_to_char(int scancode) {
    int ch = 0;

    if (scancode == 0 || scancode == 0xE0) {
        if (scancode == 0xE0) g_extended = 1;
        return 0;
    }

    if (scancode & 0x80) {
        int released = scancode & 0x7F;
        if (released == SCAN_CODE_KEY_LEFT_SHIFT || released == SCAN_CODE_KEY_RIGHT_SHIFT) {
            g_shift_pressed = FALSE;
        }
        if (released == SCAN_CODE_KEY_LEFT_CTRL || released == SCAN_CODE_KEY_RIGHT_CTRL) {
            g_ctrl_pressed = FALSE;
        }
        g_extended = 0;
        return 0;
    }

    if (scancode == SCAN_CODE_KEY_LEFT_SHIFT || scancode == SCAN_CODE_KEY_RIGHT_SHIFT) {
        g_shift_pressed = TRUE;
        return 0;
    }
    if (scancode == SCAN_CODE_KEY_LEFT_CTRL || scancode == SCAN_CODE_KEY_RIGHT_CTRL) {
        g_ctrl_pressed = TRUE;
        return 0;
    }
    if (scancode == SCAN_CODE_KEY_CAPS_LOCK) {
        g_caps_lock = !g_caps_lock;
        return 0;
    }

    if (g_extended) {
        if (scancode == 0x48) ch = 0x80;
        else if (scancode == 0x50) ch = 0x81;
        else if (scancode == 0x4B) ch = 0x82;
        else if (scancode == 0x4D) ch = 0x83;
        g_extended = 0;
    } else if (scancode < 128) {
        ch = g_scan_code_chars[scancode];
        if (ch >= 'a' && ch <= 'z') {
            if (g_caps_lock != g_shift_pressed) ch -= 32;
        } else if (g_shift_pressed) {
            ch = alternate_chars((char)ch);
        }
        if (g_ctrl_pressed && ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))) {
            char lo = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : (char)ch;
            ch = (lo - 'a') + 1;
        }
    }

    return ch;
}

char alternate_chars(char ch) {
    switch(ch) {
        case '`': return '~';
        case '1': return '!';
        case '2': return '@';
        case '3': return '#';
        case '4': return '$';
        case '5': return '%';
        case '6': return '^';
        case '7': return '&';
        case '8': return '*';
        case '9': return '(';
        case '0': return ')';
        case '-': return '_';
        case '=': return '+';
        case '[': return '{';
        case ']': return '}';
        case '\\': return '|';
        case ';': return ':';
        case '\'': return '\"';
        case ',': return '<';
        case '.': return '>';
        case '/': return '?';
        default: return ch;
    }
}

void keyboard_handler(REGISTERS *r) {
    int scancode;
    int ch = 0;

    (void)r;

    // Read scancode
    scancode = get_scancode();
    if (scancode == 0) return;
    mouse_emulate_from_scancode(scancode);
    if (g_skip_irq_scancode != 0 && scancode == g_skip_irq_scancode) {
        g_skip_irq_scancode = 0;
        return;
    }
    g_scan_code = scancode;
    g_keyboard_irq_count++;
    g_keyboard_last_tsc = rdtsc64();

    ch = decode_scancode_to_char(scancode);

    // Push decoded input to tty buffer.
    if (ch != 0) {
        if (ch == 3) g_sigint_pending++;
        if (ch == 0x80 || ch == 0x81 || ch == 0x82 || ch == 0x83) {
            /* Linux tty-compatible arrows: ESC [ A/B/C/D */
            kbd_buf_push_char(27);
            kbd_buf_push_char('[');
            if (ch == 0x80) kbd_buf_push_char('A');
            else if (ch == 0x81) kbd_buf_push_char('B');
            else if (ch == 0x82) kbd_buf_push_char('D');
            else kbd_buf_push_char('C');
            g_ch = 27;
        } else {
            g_ch = (char)ch; // legacy global compatibility
            kbd_buf_push_char((char)ch);
        }
    }
}



void keyboard_init() {
    g_caps_lock = FALSE;
    g_shift_pressed = FALSE;
    g_ctrl_pressed = FALSE;
    g_ch = 0;
    g_scan_code = 0;
    g_extended = 0;
    g_keyboard_irq_count = 0;
    g_keyboard_last_tsc = rdtsc64();
    g_sigint_pending = 0;
    kbd_head = 0;
    kbd_tail = 0;
    mouse_head = 0;
    mouse_tail = 0;
    mouse_event_head = 0;
    mouse_event_tail = 0;
    g_mouse_buttons = 0;
    g_mouse_ext_prefix = 0;
    isr_register_interrupt_handler(IRQ_BASE + 1, keyboard_handler);
}

// A blocking character read
char kb_getchar() {
    char c;

    while(g_ch <= 0) {
        __asm__ __volatile__("sti; hlt");
    }
    c = g_ch;
    g_ch = 0;
    g_scan_code = 0;
    return c;
}

char kb_get_scancode() {
    char code;

    while(g_scan_code <= 0) {
        __asm__ __volatile__("sti; hlt");
    }
    code = g_scan_code;
    g_ch = 0;
    g_scan_code = 0;
    return code;
}

int kb_read_event(char *ch, char *scan, int blocking) {
    while (g_scan_code == 0) {
        if (!blocking) return 0;
        __asm__ __volatile__("sti; hlt");
    }
    if (ch) *ch = g_ch;
    if (scan) *scan = g_scan_code;
    g_ch = 0;
    g_scan_code = 0;
    return 1;
}

int keyboard_getchar(void) {
    int ch;
    unsigned long flags;

    // Critical section: disable interrupts while modifying tail
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    
    if (kbd_head == kbd_tail) {
        // Restore interrupts
        __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory");
        return -1; 
    }

    ch = (unsigned char)kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;

    __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory");

    return ch;
}

int keyboard_pollchar(void) {
    return keyboard_getchar();
}

int keyboard_haschar(void) {
    int has;
    unsigned long flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    has = (kbd_head != kbd_tail) ? 1 : 0;
    __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory");
    if (has) return 1;
    return (inportb(KEYBOARD_STATUS_PORT) & 0x01) ? 1 : 0;
}

int keyboard_mouse_pending(void) {
    int n;
    unsigned long flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    n = mouse_head - mouse_tail;
    if (n < 0) n += MOUSE_BUF_SIZE;
    __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory");
    return n;
}

int keyboard_mouse_read(char *out, uint32_t max, int blocking) {
    uint32_t n = 0;
    if (!out || max == 0) return 0;
    while (n < max) {
        int have;
        unsigned long flags;
        __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        have = (mouse_head != mouse_tail);
        if (have) {
            out[n++] = (char)mouse_buf[mouse_tail];
            mouse_tail = (mouse_tail + 1) % MOUSE_BUF_SIZE;
        }
        __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory");
        if (!have) {
            if (n > 0 || !blocking) break;
            __asm__ __volatile__("sti; hlt");
        }
    }
    return (int)n;
}

int keyboard_mouse_event_pending(void) {
    int n;
    unsigned long flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    n = mouse_event_head - mouse_event_tail;
    if (n < 0) n += MOUSE_EVENT_BUF_SIZE;
    __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory");
    return n;
}

int keyboard_mouse_event_read(char *out, uint32_t max, int blocking) {
    uint32_t n = 0;
    if (!out || max == 0) return 0;
    while (n < max) {
        int have;
        unsigned long flags;
        __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        have = (mouse_event_head != mouse_event_tail);
        if (have) {
            out[n++] = (char)mouse_event_buf[mouse_event_tail];
            mouse_event_tail = (mouse_event_tail + 1) % MOUSE_EVENT_BUF_SIZE;
        }
        __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory");
        if (!have) {
            if (n > 0 || !blocking) break;
            __asm__ __volatile__("sti; hlt");
        }
    }
    return (int)n;
}

uint32_t keyboard_take_sigint_pending(void) {
    uint32_t v;
    unsigned long flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    v = g_sigint_pending;
    g_sigint_pending = 0;
    __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory");
    return v;
}

uint64_t keyboard_entropy_irq_count(void) {
    return g_keyboard_irq_count;
}

uint64_t keyboard_entropy_last_tsc(void) {
    return g_keyboard_last_tsc;
}
