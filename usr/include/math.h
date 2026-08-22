#ifndef MATH_H
#define MATH_H
/* DOOM's renderer is fixed-point; the only float maths it does is a
 * handful of calls in tables/menu code. These are enough for that, and
 * kept simple rather than accurate — nothing here feeds the renderer. */
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double sqrt(double x);
double pow(double b, double e);
double exp(double x);
double log(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double a, double b);
#define M_PI 3.14159265358979323846
#endif
