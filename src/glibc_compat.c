#if defined(__linux__) && defined(__x86_64__)
#include <features.h>
#if defined(__GLIBC__)
#include <stdarg.h>

__asm__(".symver compat_acosf, acosf@GLIBC_2.2.5");
__asm__(".symver compat_atan2f, atan2f@GLIBC_2.2.5");
__asm__(".symver compat_fmodf, fmodf@GLIBC_2.2.5");
__asm__(".symver compat_vsscanf, vsscanf@GLIBC_2.2.5");
__asm__(".symver compat_strtol, strtol@GLIBC_2.2.5");
__asm__(".symver compat_strtoul, strtoul@GLIBC_2.2.5");
__asm__(".symver compat_strtoull, strtoull@GLIBC_2.2.5");

extern float compat_acosf(float x);
extern float compat_atan2f(float y, float x);
extern float compat_fmodf(float x, float y);
extern int compat_vsscanf(const char* s, const char* fmt, va_list ap);
extern long compat_strtol(const char* nptr, char** endptr, int base);
extern unsigned long compat_strtoul(const char* nptr, char** endptr, int base);
extern unsigned long long compat_strtoull(const char* nptr, char** endptr, int base);

__attribute__((used)) float __wrap_acosf(float x) { return compat_acosf(x); }
__attribute__((used)) float __wrap_atan2f(float y, float x) { return compat_atan2f(y, x); }
__attribute__((used)) float __wrap_fmodf(float x, float y) { return compat_fmodf(x, y); }

__attribute__((used)) int __wrap___isoc23_sscanf(const char* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = compat_vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

__attribute__((used)) long __wrap___isoc23_strtol(const char* nptr, char** endptr, int base) {
    return compat_strtol(nptr, endptr, base);
}

__attribute__((used)) unsigned long __wrap___isoc23_strtoul(const char* nptr, char** endptr, int base) {
    return compat_strtoul(nptr, endptr, base);
}

__attribute__((used)) unsigned long long __wrap___isoc23_strtoull(const char* nptr, char** endptr, int base) {
    return compat_strtoull(nptr, endptr, base);
}
#endif
#endif
