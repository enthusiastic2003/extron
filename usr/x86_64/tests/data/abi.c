/* ABI cross-check: the same program built by gcc (host, glibc) and tcc (Extron, mlibc,
 * which was itself built by gcc) must print identical output. */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct s_small { int a; char b; };
struct s_two   { long a, b; };
struct s_dbl   { double d; int i; };
struct s_flt3  { float x, y, z; };
struct s_mix   { float f; int i; double d; };
struct s_big   { char buf[40]; int n; };
union  u_mix   { int i; float f; double d; };

static struct s_small  mk_small(int a, char b) { struct s_small s = { a, b }; return s; }
static struct s_two    mk_two(long a) { struct s_two s = { a, a * 2 }; return s; }
static struct s_dbl    mk_dbl(double d) { struct s_dbl s = { d * 1.5, 7 }; return s; }
static struct s_flt3   mk_flt3(float v) { struct s_flt3 s = { v, v + 1, v + 2 }; return s; }
static struct s_mix    mk_mix(int i) { struct s_mix s = { 0.5f, i, 2.25 }; return s; }
static struct s_big    mk_big(int n) { struct s_big s; memset(&s, 0, sizeof s); strcpy(s.buf, "big-struct"); s.n = n; return s; }
static long sum_structs(struct s_small a, struct s_two b, struct s_dbl c, struct s_flt3 d, struct s_mix e, struct s_big f) {
    return a.a + a.b + b.a + b.b + (long)c.d + c.i + (long)(d.x + d.y + d.z) + (long)e.f + e.i + (long)e.d + f.n + (long)strlen(f.buf);
}
static double many(int a, int b, int c, int d, int e, int f, int g, int h, double x1, double x2, double x3,
                   double x4, double x5, double x6, double x7, double x8, double x9, double x10) {
    return a + b + c + d + e + f + g + h + x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8 + x9 + x10;
}
static long double ld_scale(long double v, int k) { return v * k; }
static float  f_ret(float a, float b) { return a / b; }

static double va_sum(int n, ...) {
    va_list ap; va_start(ap, n);
    double t = 0;
    for (int i = 0; i < n; i++) t += (i % 3 == 0) ? va_arg(ap, int) : (i % 3 == 1) ? va_arg(ap, double) : (double)va_arg(ap, long);
    va_end(ap);
    return t;
}
static int vwrap(char *buf, size_t n, const char *fmt, ...) {   /* tcc builds the va_list, libc reads it */
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static jmp_buf jb;
static void jumper(int d) { if (d == 0) longjmp(jb, 42); jumper(d - 1); }
static void *thr(void *arg) { errno = 5; return (void *)(long)(errno + (long)arg); }

int main(void) {
    char buf[256];
    puts("-- structs by value");
    struct s_small a = mk_small(3, 'x'); struct s_two b = mk_two(21); struct s_dbl c = mk_dbl(4.0);
    struct s_flt3 d = mk_flt3(1.5f); struct s_mix e = mk_mix(9); struct s_big f = mk_big(11);
    printf("%d %c | %ld %ld | %.2f %d | %.1f %.1f %.1f | %.2f %d %.2f | %s %d\n", a.a, a.b, b.a, b.b, c.d, c.i,
           d.x, d.y, d.z, e.f, e.i, e.d, f.buf, f.n);
    printf("sum=%ld\n", sum_structs(a, b, c, d, e, f));
    puts("-- many args");
    printf("%.3f\n", many(1, 2, 3, 4, 5, 6, 7, 8, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5));
    printf("%.4Lf %.4f\n", ld_scale(1.0L / 3, 3), f_ret(1.0f, 3.0f));
    puts("-- varargs, built here and read by libc");
    printf("%.2f\n", va_sum(9, 1, 2.5, 3L, 4, 5.5, 6L, 7, 8.5, 9L));
    int r = vwrap(buf, sizeof buf, "%d %s %f %ld %c %5.2f %x %lu %Lf %d %d %d %d %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f",
                  -5, "str", 3.25, 123456789012L, 'z', 2.5, 255, 4000000000UL, 1.5L, 1, 2, 3, 4,
                  1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);
    printf("%d [%s]\n", r, buf);
    puts("-- callbacks from libc into this code");
    int arr[] = { 9, 3, 7, 1, 8, 2, 6, 4, 5, 0 };
    qsort(arr, 10, sizeof(int), cmp_int);
    int key = 7, *hit = bsearch(&key, arr, 10, sizeof(int), cmp_int);
    for (int i = 0; i < 10; i++) printf("%d", arr[i]);
    printf(" found=%d\n", hit ? (int)(hit - arr) : -1);
    puts("-- setjmp/longjmp, errno, strto*");
    int j = setjmp(jb);
    if (j == 0) jumper(5);
    printf("longjmp value %d\n", j);
    errno = 0; long big = strtol("99999999999999999999", NULL, 10);
    printf("strtol %ld errno=%d\n", big, errno == ERANGE);
    printf("strtod %.6f strtold %.6Lf\n", strtod("3.14159265358979", NULL), strtold("2.71828182845904523", NULL));
    puts("-- threads, per-thread errno");
    pthread_t t; void *rv; errno = 0;
    pthread_create(&t, NULL, thr, (void *)100);
    pthread_join(t, &rv);
    printf("thread result %ld, main errno still %d\n", (long)rv, errno);
    union u_mix u; u.d = 1.0; printf("union %lx\n", (unsigned long)(*(unsigned long long *)&u));
    printf("sizeof %zu %zu %zu %zu %zu %zu %zu\n", sizeof(struct s_small), sizeof(struct s_two), sizeof(struct s_dbl),
           sizeof(struct s_flt3), sizeof(struct s_mix), sizeof(struct s_big), sizeof(long double));
    puts("ABI OK");
    return 0;
}
