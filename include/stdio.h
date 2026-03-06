#ifndef STDIO_H
#define STDIO_H

#include <stdarg.h>

int sscanf(const char *str, const char *format, int *num1, char *op, int *num2);


int putchar(int c);                        
//int vprintf(const char *fmt, va_list ap);  
void printf(const char *fmt, ...);

#endif // STDIO_H
