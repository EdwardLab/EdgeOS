#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include "stdint.h"
#include "types.h"
#include "stdio.h"

typedef int8_t s8_t;
typedef uint8_t u8_t;
typedef int16_t s16_t;
typedef uint16_t u16_t;
typedef int32_t s32_t;
typedef uint32_t u32_t;
typedef uintptr_t mem_ptr_t;

typedef int sys_prot_t;

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_ERR_T  int

u32_t sys_now(void);

#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("[lwip] assert: %s\n", x); } while (0)

#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

#endif
