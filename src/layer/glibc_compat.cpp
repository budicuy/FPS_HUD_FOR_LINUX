#if defined(__linux__) && defined(__x86_64__)
#include <features.h>
#if defined(__GLIBC__)
#include <cstdarg>
#include <cstdint>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {

__asm__(".symver compat_acosf, acosf@GLIBC_2.2.5");
__asm__(".symver compat_atan2f, atan2f@GLIBC_2.2.5");
__asm__(".symver compat_fmod, fmod@GLIBC_2.2.5");
__asm__(".symver compat_fmodf, fmodf@GLIBC_2.2.5");
__asm__(".symver compat_sinh, sinh@GLIBC_2.2.5");
__asm__(".symver compat_cosh, cosh@GLIBC_2.2.5");
__asm__(".symver compat_sqrtf, sqrtf@GLIBC_2.2.5");
__asm__(".symver compat_vsscanf, vsscanf@GLIBC_2.2.5");
__asm__(".symver compat_strtol, strtol@GLIBC_2.2.5");
__asm__(".symver compat_strtoul, strtoul@GLIBC_2.2.5");
__asm__(".symver compat_strtoull, strtoull@GLIBC_2.2.5");
__asm__(".symver compat_strtoll, strtoll@GLIBC_2.2.5");

extern float compat_acosf(float x);
extern float compat_atan2f(float y, float x);
extern double compat_fmod(double x, double y);
extern float compat_fmodf(float x, float y);
extern double compat_sinh(double x);
extern double compat_cosh(double x);
extern float compat_sqrtf(float x);
extern int compat_vsscanf(const char* s, const char* fmt, va_list ap);
extern long compat_strtol(const char* nptr, char** endptr, int base);
extern unsigned long compat_strtoul(const char* nptr, char** endptr, int base);
extern unsigned long long compat_strtoull(const char* nptr, char** endptr, int base);
extern long long compat_strtoll(const char* nptr, char** endptr, int base);

// NOLINTBEGIN(bugprone-reserved-identifier)
__attribute__((used)) float __wrap_acosf(float x) { return compat_acosf(x); }
__attribute__((used)) float __wrap_atan2f(float y, float x) { return compat_atan2f(y, x); }
__attribute__((used)) double __wrap_fmod(double x, double y) { return compat_fmod(x, y); }
__attribute__((used)) float __wrap_fmodf(float x, float y) { return compat_fmodf(x, y); }
__attribute__((used)) double __wrap_sinh(double x) { return compat_sinh(x); }
__attribute__((used)) double __wrap_cosh(double x) { return compat_cosh(x); }
__attribute__((used)) float __wrap_sqrtf(float x) { return compat_sqrtf(x); }

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

__attribute__((used)) unsigned long __wrap___isoc23_strtoul(const char* nptr, char** endptr,
                                                            int base) {
  return compat_strtoul(nptr, endptr, base);
}

__attribute__((used)) unsigned long long __wrap___isoc23_strtoull(const char* nptr, char** endptr,
                                                                  int base) {
  return compat_strtoull(nptr, endptr, base);
}

__attribute__((used)) long long __wrap___isoc23_strtoll(const char* nptr, char** endptr, int base) {
  return compat_strtoll(nptr, endptr, base);
}

/* Static libstdc++ 16 under Clang (random.o, pulled in by std::random_device /
 * std::filesystem temp helpers) references arc4random@GLIBC_2.36. Provide a
 * GLIBC_2.2.5-only replacement backed by the getrandom(2) syscall. */
__attribute__((used)) uint32_t __wrap_arc4random(void) {
  // NOLINTEND(bugprone-reserved-identifier)
  uint32_t val = 0;
#if defined(SYS_getrandom)
  if (syscall(SYS_getrandom, &val, sizeof(val), 0) == static_cast<long>(sizeof(val))) {
    return val;
  }
#endif
  /* Fallback for kernels < 3.17 / seccomp-filtered sandboxes: mix clock + pid */
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t x = (static_cast<uint64_t>(ts.tv_sec) * 1000000007ULL) ^
               static_cast<uint64_t>(ts.tv_nsec) ^ (static_cast<uint64_t>(getpid()) << 32u) ^
               static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&val));
  x ^= x >> 33u;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33u;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33u;
  return static_cast<uint32_t>(x);
}

}  // extern "C"

#endif
#endif
