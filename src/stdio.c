#include "stdio.h"

int sscanf(const char *str, const char *format, int *num1, char *op, int *num2) {
    const char *p = str;

    *num1 = 0;
    int sign = 1;

    if (*p == '-') {
        sign = -1;
        p++;
    }

    while (*p >= '0' && *p <= '9') {
        *num1 = (*num1 * 10) + (*p - '0');
        p++;
    }
    *num1 *= sign;

    while (*p == ' ') p++;

    *op = *p++;

    while (*p == ' ') p++;

    *num2 = 0;
    sign = 1;

    if (*p == '-') {
        sign = -1;
        p++;
    }

    while (*p >= '0' && *p <= '9') {
        *num2 = (*num2 * 10) + (*p - '0');
        p++;
    }
    *num2 *= sign;

    return 3;
}
