#include "idt.h"
#include "isr.h"
#include "8259_pic.h"

IDT g_idt[NO_IDT_DESCRIPTORS];
IDT_PTR g_idt_ptr;

void idt_set_entry(int index, uint64 base, uint16 seg_sel, uint8 flags) {
    IDT *this = &g_idt[index];
    this->offset_low = (uint16)(base & 0xFFFFu);
    this->segment_selector = seg_sel;
    this->ist = 0;
    this->type_attr = flags;
    this->offset_mid = (uint16)((base >> 16) & 0xFFFFu);
    this->offset_high = (uint32)((base >> 32) & 0xFFFFFFFFu);
    this->zero = 0;
}

void idt_init(void) {
    g_idt_ptr.base_address = (uint64)g_idt;
    g_idt_ptr.limit = sizeof(g_idt) - 1;
    pic8259_init();

    idt_set_entry(0, (uint64)exception_0, 0x08, 0x8E);
    idt_set_entry(1, (uint64)exception_1, 0x08, 0x8E);
    idt_set_entry(2, (uint64)exception_2, 0x08, 0x8E);
    idt_set_entry(3, (uint64)exception_3, 0x08, 0x8E);
    idt_set_entry(4, (uint64)exception_4, 0x08, 0x8E);
    idt_set_entry(5, (uint64)exception_5, 0x08, 0x8E);
    idt_set_entry(6, (uint64)exception_6, 0x08, 0x8E);
    idt_set_entry(7, (uint64)exception_7, 0x08, 0x8E);
    idt_set_entry(8, (uint64)exception_8, 0x08, 0x8E);
    idt_set_entry(9, (uint64)exception_9, 0x08, 0x8E);
    idt_set_entry(10, (uint64)exception_10, 0x08, 0x8E);
    idt_set_entry(11, (uint64)exception_11, 0x08, 0x8E);
    idt_set_entry(12, (uint64)exception_12, 0x08, 0x8E);
    idt_set_entry(13, (uint64)exception_13, 0x08, 0x8E);
    idt_set_entry(14, (uint64)exception_14, 0x08, 0x8E);
    idt_set_entry(15, (uint64)exception_15, 0x08, 0x8E);
    idt_set_entry(16, (uint64)exception_16, 0x08, 0x8E);
    idt_set_entry(17, (uint64)exception_17, 0x08, 0x8E);
    idt_set_entry(18, (uint64)exception_18, 0x08, 0x8E);
    idt_set_entry(19, (uint64)exception_19, 0x08, 0x8E);
    idt_set_entry(20, (uint64)exception_20, 0x08, 0x8E);
    idt_set_entry(21, (uint64)exception_21, 0x08, 0x8E);
    idt_set_entry(22, (uint64)exception_22, 0x08, 0x8E);
    idt_set_entry(23, (uint64)exception_23, 0x08, 0x8E);
    idt_set_entry(24, (uint64)exception_24, 0x08, 0x8E);
    idt_set_entry(25, (uint64)exception_25, 0x08, 0x8E);
    idt_set_entry(26, (uint64)exception_26, 0x08, 0x8E);
    idt_set_entry(27, (uint64)exception_27, 0x08, 0x8E);
    idt_set_entry(28, (uint64)exception_28, 0x08, 0x8E);
    idt_set_entry(29, (uint64)exception_29, 0x08, 0x8E);
    idt_set_entry(30, (uint64)exception_30, 0x08, 0x8E);
    idt_set_entry(31, (uint64)exception_31, 0x08, 0x8E);
    idt_set_entry(32, (uint64)irq_0, 0x08, 0x8E);
    idt_set_entry(33, (uint64)irq_1, 0x08, 0x8E);
    idt_set_entry(34, (uint64)irq_2, 0x08, 0x8E);
    idt_set_entry(35, (uint64)irq_3, 0x08, 0x8E);
    idt_set_entry(36, (uint64)irq_4, 0x08, 0x8E);
    idt_set_entry(37, (uint64)irq_5, 0x08, 0x8E);
    idt_set_entry(38, (uint64)irq_6, 0x08, 0x8E);
    idt_set_entry(39, (uint64)irq_7, 0x08, 0x8E);
    idt_set_entry(40, (uint64)irq_8, 0x08, 0x8E);
    idt_set_entry(41, (uint64)irq_9, 0x08, 0x8E);
    idt_set_entry(42, (uint64)irq_10, 0x08, 0x8E);
    idt_set_entry(43, (uint64)irq_11, 0x08, 0x8E);
    idt_set_entry(44, (uint64)irq_12, 0x08, 0x8E);
    idt_set_entry(45, (uint64)irq_13, 0x08, 0x8E);
    idt_set_entry(46, (uint64)irq_14, 0x08, 0x8E);
    idt_set_entry(47, (uint64)irq_15, 0x08, 0x8E);
    idt_set_entry(128, (uint64)exception_128, 0x08, 0xEF);

    load_idt((uint64)&g_idt_ptr);
    asm volatile("sti");
}
