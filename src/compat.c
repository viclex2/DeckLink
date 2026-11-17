/* compat.c — para que el programa funcione con glibc anteriores a 2.38
 * (se compila SIN _GNU_SOURCE para usar las versiones clásicas) */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

long __isoc23_strtol(const char *n, char **e, int b) { return strtol(n, e, b); }
unsigned long __isoc23_strtoul(const char *n, char **e, int b) { return strtoul(n, e, b); }
long long __isoc23_strtoll(const char *n, char **e, int b) { return strtoll(n, e, b); }
unsigned long long __isoc23_strtoull(const char *n, char **e, int b) { return strtoull(n, e, b); }
int __isoc23_vsscanf(const char *s, const char *f, va_list ap) { return vsscanf(s, f, ap); }
int __isoc23_sscanf(const char *s, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int r = vsscanf(s, f, ap);
    va_end(ap);
    return r;
}

size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t strlcat(char *d, const char *s, size_t n)
{
    size_t dl = 0;
    while (dl < n && d[dl]) dl++;
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
size_t wcslcpy(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t l = wcslen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; wmemcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t wcslcat(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t dl = 0;
    while (dl < n && d[dl]) dl++;
    if (dl == n) return n + wcslen(s);
    return dl + wcslcpy(d + dl, s, n - dl);
}

double fmod_clasico(double, double);
float fmodf_clasico(float, float);
__asm__(".symver fmod_clasico,fmod@GLIBC_2.2.5");
__asm__(".symver fmodf_clasico,fmodf@GLIBC_2.2.5");
double fmod(double x, double y) { return fmod_clasico(x, y); }
float fmodf(float x, float y) { return fmodf_clasico(x, y); }
