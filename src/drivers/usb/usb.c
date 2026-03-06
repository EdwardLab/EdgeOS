#include "drivers/usb.h"
#include "drivers/uhci.h"
#include "drivers/xhci.h"
#include "drivers/usb_dma.h"

#include "io_ports.h"
#include "keyboard.h"
#include "stdio.h"
#include "string.h"

/* Minimal USB subsystem foundation:
 * - PCI host controller discovery (UHCI/OHCI/EHCI/XHCI)
 * - Poll hook for future controller state machines
 * - Shared mouse injection path into /dev/input/mice
 *
 * This does not yet implement transfer scheduling/enumeration/HID.
 */

#define PCI_CFG_ADDR_PORT 0xCF8u
#define PCI_CFG_DATA_PORT 0xCFCu

#define USB_PCI_CLASS_SERIAL 0x0Cu
#define USB_PCI_SUBCLASS_USB 0x03u

#define USB_PROGIF_UHCI 0x00u
#define USB_PROGIF_OHCI 0x10u
#define USB_PROGIF_EHCI 0x20u
#define USB_PROGIF_XHCI 0x30u

#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_REQ_SET_ADDRESS 5u
#define USB_REQ_SET_CONFIGURATION 9u
#define USB_REQ_SET_IDLE 10u
#define USB_REQ_GET_STATUS 0u
#define USB_REQ_CLEAR_FEATURE 1u
#define USB_REQ_SET_FEATURE 3u
#define USB_DT_DEVICE 1u
#define USB_DT_CONFIG 2u
#define USB_DT_HUB 0x29u
#define USB_CLASS_HUB 9u
#define USB_CLASS_HID 3u
#define USB_PORT_FEAT_RESET 4u
#define USB_PORT_FEAT_POWER 8u
#define USB_PORT_FEAT_C_RESET 20u
#define USB_PORT_FEAT_C_CONNECTION 16u
#define USB_PORTSTAT_CONNECTION 0x0001u
#define USB_PORTSTAT_LOWSPEED 0x0200u
#define USB_MAX_ENUM_DEPTH 2

typedef struct {
    int used;
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
    uint8_t prog_if;
    uint8_t irq_line;
    uint16_t vendor;
    uint16_t device;
    uint32_t bar0;
    uint32_t bar1;
    int kind; /* 1=UHCI, 2=OHCI, 3=EHCI, 4=XHCI */
    int active;
    uhci_controller_t uhci;
    xhci_controller_t xhci;
    int mouse_ready;
    int mouse_low_speed;
    uint8_t mouse_addr;
    uint8_t mouse_iface;
    uint8_t mouse_ep;
    uint8_t mouse_report_len;
    uhci_intr_queue_t mouse_q;
} usb_controller_t;

static usb_controller_t g_usb_ctrls[8];
static int g_usb_ctrl_count;
static int g_usb_have_mouse;
static uint32_t g_usb_poll_ticks;
static uint8_t g_usb_next_addr = 1;
static int g_usb_primary_kind; /* 0=none,1=UHCI,4=XHCI */
static int usb_enumerate_uhci_addr(usb_controller_t *ctrl, int low_speed, uint8_t addr, int depth);

static uint8_t usb_alloc_addr(void) {
    if (g_usb_next_addr < 1) g_usb_next_addr = 1;
    if (g_usb_next_addr >= 127) return 0;
    return g_usb_next_addr++;
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    ((uint32_t)off & 0xFCu);
    outportl(PCI_CFG_ADDR_PORT, addr);
    return inportl(PCI_CFG_DATA_PORT);
}

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} usb_dev_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} usb_cfg_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_if_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} usb_ep_desc_t;

static int usb_uhci_ctrl_get_desc(uhci_controller_t *hc, int low_speed, uint8_t addr,
                                  uint8_t dtype, uint8_t dindex, void *buf, uint16_t len) {
    return uhci_control_transfer(hc, low_speed, addr, 0x80u, USB_REQ_GET_DESCRIPTOR,
                                 (uint16_t)(((uint16_t)dtype << 8) | dindex), 0,
                                 buf, len, 1);
}

static int usb_uhci_ctrl_set_address(uhci_controller_t *hc, int low_speed, uint8_t old_addr, uint8_t new_addr) {
    return uhci_control_transfer(hc, low_speed, old_addr, 0x00u, USB_REQ_SET_ADDRESS,
                                 new_addr, 0, 0, 0, 0);
}

static int usb_uhci_ctrl_set_config(uhci_controller_t *hc, int low_speed, uint8_t addr, uint8_t cfgval) {
    return uhci_control_transfer(hc, low_speed, addr, 0x00u, USB_REQ_SET_CONFIGURATION,
                                 cfgval, 0, 0, 0, 0);
}

static int usb_uhci_hub_port_req(uhci_controller_t *hc, int low_speed, uint8_t hub_addr,
                                 uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                                 void *buf, uint16_t len, int in_dir) {
    uint8_t bm = in_dir ? 0xA3u : 0x23u; /* class | other(port), dir */
    return uhci_control_transfer(hc, low_speed, hub_addr, bm, bRequest, wValue, wIndex, buf, len, in_dir);
}

static int usb_uhci_hub_port_status(uhci_controller_t *hc, int low_speed, uint8_t hub_addr,
                                    uint8_t port, uint16_t *status, uint16_t *change) {
    uint8_t st[4];
    if (!status || !change) return -1;
    memset(st, 0, sizeof(st));
    if (usb_uhci_hub_port_req(hc, low_speed, hub_addr, USB_REQ_GET_STATUS, 0, port, st, sizeof(st), 1) < 0) return -1;
    *status = (uint16_t)st[0] | ((uint16_t)st[1] << 8);
    *change = (uint16_t)st[2] | ((uint16_t)st[3] << 8);
    return 0;
}

static void usb_enumerate_uhci_hub_children(usb_controller_t *ctrl, int hub_low_speed, uint8_t hub_addr,
                                            uint8_t ports, int depth) {
    uhci_controller_t *hc;
    if (!ctrl || depth > USB_MAX_ENUM_DEPTH || ports == 0) return;
    hc = &ctrl->uhci;
    for (uint8_t p = 1; p <= ports; ++p) {
        uint16_t st = 0, ch = 0;
        uint8_t addr = 0;
        int child_low_speed = 0;

        (void)usb_uhci_hub_port_req(hc, hub_low_speed, hub_addr, USB_REQ_SET_FEATURE, USB_PORT_FEAT_POWER, p, 0, 0, 0);
        for (volatile int d = 0; d < 200000; ++d) { (void)d; }

        if (usb_uhci_hub_port_status(hc, hub_low_speed, hub_addr, p, &st, &ch) < 0) continue;
        if ((st & USB_PORTSTAT_CONNECTION) == 0) continue;

        (void)usb_uhci_hub_port_req(hc, hub_low_speed, hub_addr, USB_REQ_SET_FEATURE, USB_PORT_FEAT_RESET, p, 0, 0, 0);
        for (volatile int d = 0; d < 300000; ++d) { (void)d; }
        (void)usb_uhci_hub_port_req(hc, hub_low_speed, hub_addr, USB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_RESET, p, 0, 0, 0);
        (void)usb_uhci_hub_port_req(hc, hub_low_speed, hub_addr, USB_REQ_CLEAR_FEATURE, USB_PORT_FEAT_C_CONNECTION, p, 0, 0, 0);

        if (usb_uhci_hub_port_status(hc, hub_low_speed, hub_addr, p, &st, &ch) < 0) continue;
        child_low_speed = (st & USB_PORTSTAT_LOWSPEED) ? 1 : 0;
        addr = usb_alloc_addr();
        if (addr == 0) {
            printf("[usb][hub] no free USB addresses for downstream device on port %u\n", (uint32_t)p);
            continue;
        }

        /* Enumeration always starts at address 0 before SET_ADDRESS. */
        {
            usb_dev_desc_t dd;
            memset(&dd, 0, sizeof(dd));
            if (usb_uhci_ctrl_get_desc(hc, child_low_speed, 0, USB_DT_DEVICE, 0, &dd, 8) < 0) {
                printf("[usb][hub] port%u GET_DESCRIPTOR(device,8) failed\n", (uint32_t)p);
                continue;
            }
            if (usb_uhci_ctrl_set_address(hc, child_low_speed, 0, addr) < 0) {
                printf("[usb][hub] port%u SET_ADDRESS %u failed\n", (uint32_t)p, (uint32_t)addr);
                continue;
            }
            for (volatile int d = 0; d < 200000; ++d) { (void)d; }
        }

        printf("[usb][hub] downstream port%u addr=%u speed=%s\n",
               (uint32_t)p, (uint32_t)addr, child_low_speed ? "low" : "full");
        (void)usb_enumerate_uhci_addr(ctrl, child_low_speed, addr, depth + 1);
    }
}

static void usb_parse_cfg_for_classes(const uint8_t *buf, uint16_t len,
                                      int *has_boot_mouse, int *has_hub_iface) {
    uint16_t off = 0;
    if (has_boot_mouse) *has_boot_mouse = 0;
    if (has_hub_iface) *has_hub_iface = 0;
    while (off + 2 <= len) {
        uint8_t dlen = buf[off];
        uint8_t dtype = buf[off + 1];
        if (dlen < 2) break;
        if (off + dlen > len) break;
        if (dtype == 4 && dlen >= sizeof(usb_if_desc_t)) {
            const usb_if_desc_t *id = (const usb_if_desc_t *)(buf + off);
            if (id->bInterfaceClass == USB_CLASS_HUB) {
                if (has_hub_iface) *has_hub_iface = 1;
            }
            if (id->bInterfaceClass == USB_CLASS_HID &&
                id->bInterfaceSubClass == 1 && id->bInterfaceProtocol == 2) {
                if (has_boot_mouse) *has_boot_mouse = 1;
            }
        }
        off += dlen;
    }
}

static int usb_find_boot_mouse_ep(const uint8_t *buf, uint16_t len,
                                  uint8_t *iface_out, uint8_t *ep_out,
                                  uint16_t *maxpkt_out, uint8_t *interval_out) {
    uint16_t off = 0;
    int in_mouse_if = 0;
    uint8_t cur_if = 0;
    if (!buf) return -1;
    while (off + 2 <= len) {
        uint8_t dlen = buf[off];
        uint8_t dtype = buf[off + 1];
        if (dlen < 2 || off + dlen > len) break;
        if (dtype == 4 && dlen >= sizeof(usb_if_desc_t)) {
            const usb_if_desc_t *id = (const usb_if_desc_t *)(buf + off);
            cur_if = id->bInterfaceNumber;
            in_mouse_if = (id->bInterfaceClass == USB_CLASS_HID &&
                           id->bInterfaceSubClass == 1 &&
                           id->bInterfaceProtocol == 2);
        } else if (dtype == 5 && dlen >= sizeof(usb_ep_desc_t) && in_mouse_if) {
            const usb_ep_desc_t *ed = (const usb_ep_desc_t *)(buf + off);
            if ((ed->bEndpointAddress & 0x80u) && ((ed->bmAttributes & 0x03u) == 0x03u)) {
                if (iface_out) *iface_out = cur_if;
                if (ep_out) *ep_out = ed->bEndpointAddress;
                if (maxpkt_out) *maxpkt_out = (uint16_t)(ed->wMaxPacketSize & 0x07FFu);
                if (interval_out) *interval_out = ed->bInterval;
                return 0;
            }
        }
        off += dlen;
    }
    return -1;
}

static int usb_uhci_ctrl_set_protocol_boot(uhci_controller_t *hc, int low_speed, uint8_t addr, uint8_t iface_num) {
    /* HID class SET_PROTOCOL, wValue=0 => boot protocol */
    return uhci_control_transfer(hc, low_speed, addr, 0x21u, 0x0Bu, 0u, iface_num, 0, 0, 0);
}

static int usb_uhci_ctrl_set_idle(uhci_controller_t *hc, int low_speed, uint8_t addr, uint8_t iface_num,
                                  uint8_t duration, uint8_t report_id) {
    uint16_t wValue = (uint16_t)(((uint16_t)duration << 8) | report_id);
    return uhci_control_transfer(hc, low_speed, addr, 0x21u, USB_REQ_SET_IDLE, wValue, iface_num, 0, 0, 0);
}

static int usb_enumerate_uhci_addr(usb_controller_t *ctrl, int low_speed, uint8_t addr, int depth) {
    uhci_controller_t *hc;
    usb_dev_desc_t dd;
    uint8_t cfg_hdr[9];
    uint8_t cfg_buf[256];
    uint8_t hub_desc[16];
    int has_boot_mouse = 0;
    int has_hub_iface = 0;
    uint8_t mouse_iface = 0, mouse_ep = 0, mouse_interval = 10, hub_ports = 0;
    uint16_t mouse_maxpkt = 0;
    uint16_t total;

    if (!ctrl) return -1;
    hc = &ctrl->uhci;
    if (!hc || !hc->used || addr == 0) return -1;

    memset(&dd, 0, sizeof(dd));
    if (usb_uhci_ctrl_get_desc(hc, low_speed, addr, USB_DT_DEVICE, 0, &dd, sizeof(dd)) < 0) {
        printf("[usb][uhci] addr=%u GET_DESCRIPTOR(device,18) failed\n", (uint32_t)addr);
        return -1;
    }
    printf("[usb][uhci] dev addr=%u vid=%04x pid=%04x class=%u/%u/%u cfgs=%u ep0=%u\n",
           (uint32_t)addr, (uint32_t)dd.idVendor, (uint32_t)dd.idProduct,
           (uint32_t)dd.bDeviceClass, (uint32_t)dd.bDeviceSubClass, (uint32_t)dd.bDeviceProtocol,
           (uint32_t)dd.bNumConfigurations, (uint32_t)dd.bMaxPacketSize0);

    memset(cfg_hdr, 0, sizeof(cfg_hdr));
    if (usb_uhci_ctrl_get_desc(hc, low_speed, addr, USB_DT_CONFIG, 0, cfg_hdr, sizeof(cfg_hdr)) < 0) {
        printf("[usb][uhci] addr=%u GET_DESCRIPTOR(config hdr) failed\n", (uint32_t)addr);
        return 0;
    }
    total = (uint16_t)cfg_hdr[2] | ((uint16_t)cfg_hdr[3] << 8);
    if (total > sizeof(cfg_buf)) total = sizeof(cfg_buf);
    if (total < sizeof(cfg_hdr)) total = sizeof(cfg_hdr);
    memset(cfg_buf, 0, sizeof(cfg_buf));
    if (usb_uhci_ctrl_get_desc(hc, low_speed, addr, USB_DT_CONFIG, 0, cfg_buf, total) < 0) {
        printf("[usb][uhci] addr=%u GET_DESCRIPTOR(config %u) failed\n", (uint32_t)addr, (uint32_t)total);
        return 0;
    }
    usb_parse_cfg_for_classes(cfg_buf, total, &has_boot_mouse, &has_hub_iface);
    (void)usb_find_boot_mouse_ep(cfg_buf, total, &mouse_iface, &mouse_ep, &mouse_maxpkt, &mouse_interval);
    printf("[usb][uhci] config total=%u boot-mouse=%d hub-iface=%d\n",
           (uint32_t)total, has_boot_mouse, has_hub_iface);

    /* Configure the device (use first configuration) */
    if (cfg_buf[5] != 0) {
        if (usb_uhci_ctrl_set_config(hc, low_speed, addr, cfg_buf[5]) == 0) {
            printf("[usb][uhci] SET_CONFIGURATION %u ok\n", (uint32_t)cfg_buf[5]);
        } else {
            printf("[usb][uhci] SET_CONFIGURATION %u failed\n", (uint32_t)cfg_buf[5]);
        }
    }

    if (dd.bDeviceClass == USB_CLASS_HUB || has_hub_iface) {
        memset(hub_desc, 0, sizeof(hub_desc));
        if (uhci_control_transfer(hc, low_speed, addr, 0xA0u, USB_REQ_GET_DESCRIPTOR,
                                  (uint16_t)(USB_DT_HUB << 8), 0, hub_desc, 8, 1) == 0) {
            hub_ports = hub_desc[2];
            printf("[usb][hub] hub descriptor: ports=%u characteristics=0x%02x%02x\n",
                   (uint32_t)hub_desc[2], (uint32_t)hub_desc[4], (uint32_t)hub_desc[3]);
            if (depth < USB_MAX_ENUM_DEPTH) {
                usb_enumerate_uhci_hub_children(ctrl, low_speed, addr, hub_ports, depth + 1);
            }
        } else {
            printf("[usb][hub] GET_DESCRIPTOR(hub) failed\n");
        }
    }
    if (has_boot_mouse && !ctrl->mouse_ready) {
        (void)usb_uhci_ctrl_set_protocol_boot(hc, low_speed, addr, mouse_iface);
        (void)usb_uhci_ctrl_set_idle(hc, low_speed, addr, mouse_iface, 0, 0);
        if (mouse_maxpkt == 0) mouse_maxpkt = 8;
        if (mouse_maxpkt > 8) mouse_maxpkt = 8;
        printf("[usb][hid] boot mouse iface=%u ep=0x%02x maxpkt=%u interval=%u\n",
               (uint32_t)mouse_iface, (uint32_t)mouse_ep, (uint32_t)mouse_maxpkt, (uint32_t)mouse_interval);
        if (uhci_intr_queue_open(hc, low_speed, addr, mouse_ep, mouse_maxpkt, mouse_interval, &ctrl->mouse_q) == 0) {
            ctrl->mouse_ready = 1;
            ctrl->mouse_low_speed = low_speed;
            ctrl->mouse_addr = addr;
            ctrl->mouse_iface = mouse_iface;
            ctrl->mouse_ep = mouse_ep;
            ctrl->mouse_report_len = (uint8_t)mouse_maxpkt;
            printf("[usb][hid] mouse queue armed ep=0x%02x interval=%u maxpkt=%u\n",
                   (uint32_t)mouse_ep, (uint32_t)mouse_interval, (uint32_t)mouse_maxpkt);
        }
    }
    return 0;
}

static void __attribute__((unused)) usb_enumerate_uhci_root_ports(usb_controller_t *ctrl) {
    uhci_controller_t *hc;
    int found = 0;
    if (!ctrl) return;
    hc = &ctrl->uhci;
    for (int port = 0; port < 2; ++port) {
        int low_speed = 0;
        uint8_t addr = 0;
        usb_dev_desc_t dd;
        if (!uhci_port_connected(hc, port)) continue;
        found = 1;
        if (uhci_port_reset_enable(hc, port, &low_speed) < 0) {
            printf("[usb][uhci] port%d reset/enable failed\n", port + 1);
            continue;
        }
        printf("[usb][uhci] root-port%d device connected speed=%s\n",
               port + 1, low_speed ? "low" : "full");

        addr = usb_alloc_addr();
        if (addr == 0) {
            printf("[usb][uhci] out of USB addresses on root-port%d\n", port + 1);
            continue;
        }
        memset(&dd, 0, sizeof(dd));
        if (usb_uhci_ctrl_get_desc(hc, low_speed, 0, USB_DT_DEVICE, 0, &dd, 8) < 0) {
            printf("[usb][uhci] root-port%d GET_DESCRIPTOR(device,8) failed\n", port + 1);
            continue;
        }
        if (usb_uhci_ctrl_set_address(hc, low_speed, 0, addr) < 0) {
            printf("[usb][uhci] root-port%d SET_ADDRESS %u failed\n", port + 1, (uint32_t)addr);
            continue;
        }
        for (volatile int d = 0; d < 200000; ++d) { (void)d; }
        (void)usb_enumerate_uhci_addr(ctrl, low_speed, addr, 0);
    }
    if (!found) printf("[usb][uhci] no device on root ports\n");
}

void usb_hid_process_boot_report(const uint8_t *report, uint16_t n) {
    int off = 0;
    uint8_t buttons;
    int8_t dx, dy, wheel = 0;
    int wheel_present = 0;
    if (!report || n < 3) return;
    if (n >= 4 && (report[0] & 0xF8u) == 0 && (report[1] & 0xE0u) == 0) off = 1;
    if ((uint16_t)(off + 3) > n) return;
    buttons = report[off + 0] & 0x07u;
    dx = (int8_t)report[off + 1];
    dy = (int8_t)report[off + 2];
    if ((uint16_t)(off + 4) <= n) {
        wheel = (int8_t)report[off + 3];
        wheel_present = 1;
    }
    usb_hid_mouse_report_boot(dx, dy, wheel, buttons, wheel_present);
}

static void __attribute__((unused)) usb_poll_mouse_queues(void) {
    uint8_t report[16];
    uint16_t n = 0;
    for (int i = 0; i < g_usb_ctrl_count; ++i) {
        usb_controller_t *c = &g_usb_ctrls[i];
        if (!c->used || c->kind != 1 || !c->mouse_ready) continue;
        if (uhci_intr_queue_poll(&c->uhci, &c->mouse_q, report, sizeof(report), &n) <= 0) continue;
        usb_hid_process_boot_report(report, n);
    }
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint16_t)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

static uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    return (uint8_t)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

static void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    ((uint32_t)off & 0xFCu);
    outportl(PCI_CFG_ADDR_PORT, addr);
    outportl(PCI_CFG_DATA_PORT, v);
}

static void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t val) {
    uint32_t cur = pci_cfg_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    uint32_t sh = (uint32_t)(off & 2u) * 8u;
    cur = (cur & ~(0xFFFFu << sh)) | ((uint32_t)val << sh);
    pci_cfg_write32(bus, slot, func, (uint8_t)(off & 0xFCu), cur);
}

static __attribute__((unused)) const char *usb_prog_if_name(uint8_t p) {
    switch (p) {
        case USB_PROGIF_UHCI: return "UHCI";
        case USB_PROGIF_OHCI: return "OHCI";
        case USB_PROGIF_EHCI: return "EHCI";
        case USB_PROGIF_XHCI: return "XHCI";
        default: return "USB?";
    }
}

static int usb_prog_if_kind(uint8_t p) {
    switch (p) {
        case USB_PROGIF_UHCI: return 1;
        case USB_PROGIF_OHCI: return 2;
        case USB_PROGIF_EHCI: return 3;
        case USB_PROGIF_XHCI: return 4;
        default: return 0;
    }
}

static uint32_t usb_pci_pick_io_bar(uint8_t bus, uint8_t dev, uint8_t fn) {
    for (uint8_t off = 0x10; off <= 0x24; off += 4) {
        uint32_t bar = pci_cfg_read32(bus, dev, fn, off);
        if ((bar & 1u) && (bar & ~0x3u)) return bar;
    }
    return 0;
}

static void usb_try_xhci_companion_handoff(const usb_controller_t *c) {
    uint32_t usb2_mask, usb3_mask;
    uint32_t usb2_before, usb3_before;
    if (!c || c->kind != 4) return;
    /* Intel-style optional routing registers:
     * 0xD4 USB2PRM (mask), 0xD0 XUSB2PR (route to xHCI)
     * 0xDC USB3PRM (mask), 0xD8 USB3_PSSEN (enable/route to xHCI)
     */
    usb2_mask = pci_cfg_read32(c->bus, c->dev, c->fn, 0xD4);
    usb3_mask = pci_cfg_read32(c->bus, c->dev, c->fn, 0xDC);
    usb2_before = pci_cfg_read32(c->bus, c->dev, c->fn, 0xD0);
    usb3_before = pci_cfg_read32(c->bus, c->dev, c->fn, 0xD8);

    if (usb2_mask != 0 && usb2_mask != 0xFFFFFFFFu) {
        pci_cfg_write32(c->bus, c->dev, c->fn, 0xD0, usb2_mask);
    }
    if (usb3_mask != 0 && usb3_mask != 0xFFFFFFFFu) {
        pci_cfg_write32(c->bus, c->dev, c->fn, 0xD8, usb3_mask);
    }

    if ((usb2_mask != 0 && usb2_mask != 0xFFFFFFFFu) ||
        (usb3_mask != 0 && usb3_mask != 0xFFFFFFFFu)) {
        printf("[usb][xhci] companion handoff usb2: 0x%x->0x%x (mask=0x%x) usb3: 0x%x->0x%x (mask=0x%x)\n",
               usb2_before, pci_cfg_read32(c->bus, c->dev, c->fn, 0xD0), usb2_mask,
               usb3_before, pci_cfg_read32(c->bus, c->dev, c->fn, 0xD8), usb3_mask);
    }
}

static int usb_activate_controller(usb_controller_t *c) {
    if (!c || !c->used) return 0;
    if (c->active) return 1;
    c->active = 1;
    if (c->kind == 4) {
        usb_try_xhci_companion_handoff(c);
        if (xhci_init_controller(&c->xhci, c->bus, c->dev, c->fn,
                                 c->vendor, c->device, c->bar0, c->bar1, c->irq_line) < 0) {
            printf("[usb][xhci] init failed for %u:%u.%u\n",
                   (uint32_t)c->bus, (uint32_t)c->dev, (uint32_t)c->fn);
            c->active = 0;
            return 0;
        }
        return 1;
    }
    if (c->kind == 1) {
        if (uhci_init_controller(&c->uhci, c->bus, c->dev, c->fn,
                                 c->vendor, c->device, c->bar0, c->irq_line) < 0) {
            printf("[usb][uhci] init failed for %u:%u.%u\n",
                   (uint32_t)c->bus, (uint32_t)c->dev, (uint32_t)c->fn);
            c->active = 0;
            return 0;
        }
        usb_enumerate_uhci_root_ports(c);
        return 1;
    }
    if (c->kind == 3) {
        printf("[usb][ehci] controller present but EHCI driver not implemented at %u:%u.%u\n",
               (uint32_t)c->bus, (uint32_t)c->dev, (uint32_t)c->fn);
        c->active = 0;
        return 0;
    }
    if (c->kind == 2) {
        printf("[usb][ohci] controller present but OHCI driver not implemented at %u:%u.%u\n",
               (uint32_t)c->bus, (uint32_t)c->dev, (uint32_t)c->fn);
        c->active = 0;
        return 0;
    }
    c->active = 0;
    return 0;
}

static void usb_register_controller(uint8_t bus, uint8_t dev, uint8_t fn) {
    usb_controller_t *c;
    if (g_usb_ctrl_count >= (int)(sizeof(g_usb_ctrls) / sizeof(g_usb_ctrls[0]))) return;
    c = &g_usb_ctrls[g_usb_ctrl_count++];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->bus = bus;
    c->dev = dev;
    c->vendor = pci_cfg_read16(bus, dev, fn, 0x00);
    c->device = pci_cfg_read16(bus, dev, fn, 0x02);
    c->prog_if = pci_cfg_read8(bus, dev, fn, 0x09);
    c->irq_line = pci_cfg_read8(bus, dev, fn, 0x3C);
    c->bar0 = pci_cfg_read32(bus, dev, fn, 0x10);
    c->bar1 = pci_cfg_read32(bus, dev, fn, 0x14);
    c->kind = usb_prog_if_kind(c->prog_if);
    if (c->kind == 1) {
        uint16_t cmd = pci_cfg_read16(bus, dev, fn, 0x04);
        cmd |= 0x0001u; /* I/O space */
        cmd |= 0x0004u; /* bus master */
        pci_cfg_write16(bus, dev, fn, 0x04, cmd);
        c->bar0 = usb_pci_pick_io_bar(bus, dev, fn);
    }

    printf("[usb] controller %s at %u:%u.%u ven=%04x dev=%04x irq=%u bar0=0x%x\n",
           (c->kind == 4) ? "XHCI" : "LEGACY-USB",
           (uint32_t)bus, (uint32_t)dev, (uint32_t)fn,
           (uint32_t)c->vendor, (uint32_t)c->device,
           (uint32_t)c->irq_line, c->bar0);

}

static void usb_scan_pci(void) {
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t dev = 0; dev < 32; ++dev) {
            for (uint8_t fn = 0; fn < 8; ++fn) {
                uint16_t ven = pci_cfg_read16((uint8_t)bus, dev, fn, 0x00);
                uint8_t cls, sub;
                if (ven == 0xFFFFu) {
                    if (fn == 0) break;
                    continue;
                }
                cls = pci_cfg_read8((uint8_t)bus, dev, fn, 0x0B);
                sub = pci_cfg_read8((uint8_t)bus, dev, fn, 0x0A);
                if (cls == USB_PCI_CLASS_SERIAL && sub == USB_PCI_SUBCLASS_USB) {
                    usb_register_controller((uint8_t)bus, dev, fn);
                }
                if (fn == 0) {
                    uint8_t hdr = pci_cfg_read8((uint8_t)bus, dev, fn, 0x0E);
                    if ((hdr & 0x80u) == 0) break;
                }
            }
        }
    }
}

void usb_init(void) {
    int have_xhci = 0;
    int xhci_ok = 0;
    int fallback_ok = 0;
    g_usb_ctrl_count = 0;
    g_usb_have_mouse = 0;
    g_usb_poll_ticks = 0;
    g_usb_next_addr = 1;
    g_usb_primary_kind = 0;
    memset(g_usb_ctrls, 0, sizeof(g_usb_ctrls));
    usb_dma_init();
    usb_scan_pci();
    for (int i = 0; i < g_usb_ctrl_count; ++i) {
        if (g_usb_ctrls[i].used && g_usb_ctrls[i].kind == 4) {
            have_xhci = 1;
            printf("[usb] discovered xHCI controller at %u:%u.%u\n",
                   (uint32_t)g_usb_ctrls[i].bus, (uint32_t)g_usb_ctrls[i].dev, (uint32_t)g_usb_ctrls[i].fn);
        }
    }
    if (have_xhci) {
        for (int i = 0; i < g_usb_ctrl_count; ++i) {
            if (g_usb_ctrls[i].used && g_usb_ctrls[i].kind == 4) {
                if (usb_activate_controller(&g_usb_ctrls[i])) xhci_ok = 1;
            }
        }
        if (xhci_ok) {
            g_usb_primary_kind = 4;
            printf("[usb] using xHCI as primary controller\n");
        } else {
            printf("[usb] xHCI init failed, using fallback controller\n");
        }
    } else {
        printf("[usb] xHCI not present, falling back to EHCI/UHCI/OHCI\n");
    }

    if (!xhci_ok) {
        /* Priority: EHCI -> UHCI -> OHCI (only UHCI currently implemented). */
        for (int pass = 0; pass < 3 && !fallback_ok; ++pass) {
            int want_kind = (pass == 0) ? 3 : ((pass == 1) ? 1 : 2);
            for (int i = 0; i < g_usb_ctrl_count; ++i) {
                if (!g_usb_ctrls[i].used || g_usb_ctrls[i].kind != want_kind) continue;
                if (usb_activate_controller(&g_usb_ctrls[i])) {
                    fallback_ok = 1;
                    g_usb_primary_kind = want_kind;
                    break;
                }
            }
        }
    } else if (!g_usb_have_mouse) {
        /* Compatibility fallback only when needed for HID in mixed environments. */
        for (int i = 0; i < g_usb_ctrl_count; ++i) {
            if (!g_usb_ctrls[i].used || g_usb_ctrls[i].kind != 1) continue;
            if (usb_activate_controller(&g_usb_ctrls[i])) {
                fallback_ok = 1;
                printf("[usb] xHCI active but enabling UHCI compatibility fallback for HID\n");
            }
        }
    }

    if (g_usb_ctrl_count == 0) {
        printf("[usb] no PCI USB host controllers found\n");
    } else {
        if (g_usb_primary_kind == 4) {
            printf("[usb] found %d controller(s), primary=xHCI\n", g_usb_ctrl_count);
        } else if (g_usb_primary_kind == 1) {
            printf("[usb] using UHCI as primary controller\n");
        } else if (g_usb_primary_kind == 3) {
            printf("[usb] using EHCI as primary controller\n");
        } else if (g_usb_primary_kind == 2) {
            printf("[usb] using OHCI as primary controller\n");
        } else {
            printf("[usb] found %d controller(s), none initialized successfully\n", g_usb_ctrl_count);
        }
        (void)fallback_ok;
    }
}

void usb_poll(void) {
    /* Poll hook for future USB schedules/enumeration state machine.
     * Kept lightweight for now. */
    if (++g_usb_poll_ticks == 0) g_usb_poll_ticks = 1;
    for (int i = 0; i < g_usb_ctrl_count; ++i) {
        if (!g_usb_ctrls[i].used || !g_usb_ctrls[i].active) continue;
        if (g_usb_ctrls[i].kind == 1) uhci_poll_controller(&g_usb_ctrls[i].uhci);
        if (g_usb_ctrls[i].kind == 4) xhci_poll_controller(&g_usb_ctrls[i].xhci);
    }
    usb_poll_mouse_queues();
    (void)g_usb_have_mouse;
}

int usb_present_mouse(void) {
    return g_usb_have_mouse;
}

/* Future controller drivers (UHCI/EHCI/XHCI) should call this after parsing
 * HID boot mouse reports. Export kept local for now until first implementation. */
void usb_hid_mouse_report_boot(int8_t dx, int8_t dy, int8_t wheel, uint8_t buttons, int wheel_present) {
    keyboard_mouse_emit_packet_ex((int)dx, (int)dy, (int)wheel, buttons, wheel_present);
    g_usb_have_mouse = 1;
}
