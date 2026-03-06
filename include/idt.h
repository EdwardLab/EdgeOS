#ifndef IDT_H
#define IDT_H

#include "types.h"

#define NO_IDT_DESCRIPTORS 256

typedef struct {
    uint16 offset_low;
    uint16 segment_selector;
    uint8 ist;
    uint8 type_attr;
    uint16 offset_mid;
    uint32 offset_high;
    uint32 zero;
} __attribute__((packed)) IDT;

typedef struct {
    uint16 limit;
    uint64 base_address;
} __attribute__((packed)) IDT_PTR;

extern void load_idt(uint64 idt_ptr);

void idt_set_entry(int index, uint64 base, uint16 seg_sel, uint8 flags);
void idt_init(void);

#endif
