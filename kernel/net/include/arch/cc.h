#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

/* x86-64 and aarch64: little-endian, 64-bit pointers. */
#define BYTE_ORDER LITTLE_ENDIAN

typedef uint8_t  u8_t;   typedef int8_t  s8_t;
typedef uint16_t u16_t;  typedef int16_t s16_t;
typedef uint32_t u32_t;  typedef int32_t s32_t;
typedef uintptr_t mem_ptr_t;
typedef long ssize_t;            /* lwIP wants it; <unistd.h> is not available in the kernel */

#define LWIP_NO_STDDEF_H    0
#define LWIP_NO_STDINT_H    1
#define LWIP_NO_INTTYPES_H  1
#define LWIP_NO_LIMITS_H    0
#define LWIP_NO_CTYPE_H     1
#define LWIP_NO_UNISTD_H    1      /* <unistd.h> would drag in the host libc (and its sleep()) */

#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT   __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

#define lwip_isdigit(c)  ((c) >= '0' && (c) <= '9')
#define lwip_isxdigit(c) (lwip_isdigit(c) || ((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F'))
#define lwip_islower(c)  ((c) >= 'a' && (c) <= 'z')
#define lwip_isupper(c)  ((c) >= 'A' && (c) <= 'Z')
#define lwip_isspace(c)  ((c) == ' ' || ((c) >= '\t' && (c) <= '\r'))
#define lwip_tolower(c)  (lwip_isupper(c) ? (c) - 'A' + 'a' : (c))
#define lwip_toupper(c)  (lwip_islower(c) ? (c) - 'a' + 'A' : (c))

#endif
