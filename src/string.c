#include "string.h"
#include <stdint.h>
#include "types.h"

void *memset(void *dst, char c, uint32 n) {
    char *temp = dst;
    for (; n != 0; n--) *temp++ = c;
    return dst;
}

void *memcpy(void *dst, const void *src, uint32 n) {
    char *ret = dst;
    char *p = dst;
    const char *q = src;
    while (n--)
        *p++ = *q++;
    return ret;
}


void *memmove(void *dst, const void *src, uint32 n) {
    uint8 *d = (uint8*)dst;
    const uint8 *s = (const uint8*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (uint32 i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (uint32 i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *s1, const void *s2, uint32 n) {
    const uint8 *a = (const uint8 *)s1;
    const uint8 *b = (const uint8 *)s2;
    while (n--) {
        if (*a != *b) return (int)*a - (int)*b;
        a++;
        b++;
    }
    return 0;
}

int strlen(const char *s) {
    int len = 0;
    while (*s++)
        len++;
    return len;
}

int strcmp(const char *s1, char *s2) {
    int i = 0;

    while ((s1[i] == s2[i])) {
        if (s2[i++] == 0)
            return 0;
    }
    return 1;
}

int strncmp(const char *s1, const char *s2, int c) {
    int result = 0;

    while (c) {
        result = *s1 - *s2++;
        if ((result != 0) || (*s1++ == 0)) {
            break;
        }
        c--;
    }
    return result;
}

int strcpy(char *dst, const char *src) {
    int i = 0;
    while ((*dst++ = *src++) != 0)
        i++;
    return i;
}

void strcat(char *dest, const char *src) {
    char *end = (char *)dest + strlen(dest);
    memcpy((void *)end, (void *)src, strlen(src));
    end = end + strlen(src);
    *end = '\0';
}

int isspace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

int isalpha(char c) {
    return (((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')));
}

char upper(char c) {
    if ((c >= 'a') && (c <= 'z'))
        return (c - 32);
    return c;
}

char lower(char c) {
    if ((c >= 'A') && (c <= 'Z'))
        return (c + 32);
    return c;
}

void itoa(char *buf, int base, int d) {
    uint32_t u = (uint32_t)d;
    int b = 10;
    
    if (base == 'x') b = 16;
    else if (base == 'o') b = 8;
    else if (base == 'd' && d < 0) {
        *buf++ = '-';
        u = (uint32_t)-d;
    }

    char tmp[32];
    int i = 0;
    if (u == 0) tmp[i++] = '0';
    while (u > 0) {
        int r = u % b;
        tmp[i++] = (r < 10) ? (r + '0') : (r - 10 + 'a');
        u /= b;
    }
    while (i > 0) *buf++ = tmp[--i];
    *buf = 0;
}

char *strstr(const char *in, const char *str) {
    char c;
    uint32 len;

    c = *str++;
    if (!c)
        return (char *)in;

    len = strlen(str);
    do {
        char sc;
        do {
            sc = *in++;
            if (!sc)
                return (char *)0;
        } while (sc != c);
    } while (strncmp(in, str, len) != 0);

    return (char *)(in - 1);
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for ( ; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

int atoi(const char *s) {
    int sign = 1;
    int v = 0;
    if (!s) return 0;
    while (*s && isspace(*s)) s++;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return sign * v;
}
