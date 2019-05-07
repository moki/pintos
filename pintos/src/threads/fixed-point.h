#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <debug.h>

/* Parameters. */
#define FIX_BITS 32             /* Total bits per fixed-point number. */
#define FIX_P 16                /* Number of integer bits. */
#define FIX_Q 16                /* Number of fractional bits. */
#define FIX_F (1 << FIX_Q)      /* pow(2, FIX_Q). */

#define FIX_MIN_INT (-FIX_MAX_INT)      /* Smallest representable integer. */
#define FIX_MAX_INT ((1 << FIX_P) - 1)  /* Largest representable integer. */

/* ? */
#define P 17
#define Q 14
#define FRACTION 1 << (Q)

/* Fixed-point real arithmetic */
/* Here x and y are fixed-point number, n is an integer */
#define CONVERT_TO_FP(n) (n) * (FRACTION)
#define CONVERT_TO_INT_ZERO(x) (x) / (FRACTION)
#define CONVERT_TO_INT_NEAREST(x) ((x) >= 0 ? ((x) + (FRACTION) / 2)\
                                   / (FRACTION) : ((x) - (FRACTION) / 2)\
                                   / (FRACTION))
#define ADD(x, y) (x) + (y)
#define SUB(x, y) (x) - (y)
#define ADD_INT(x, n) (x) + (n) * (FRACTION)
#define SUB_INT(x, n) (x) - (n) * (FRACTION)
#define MULTIPLE(x, y) ((int64_t)(x)) * (y) / (FRACTION)
#define MULT_INT(x, n) (x) * (n)
#define DIVIDE(x, y) ((int64_t)(x)) * (FRACTION) / (y)
#define DIV_INT(x, n) (x) / (n)
/* ? */

/* A fixed-point number. */
typedef struct
  {
    int f;
  }
fixed_point_t;

/* Returns a fixed-point number with F as its internal value. */
static inline fixed_point_t
__mk_fix (int f)
{
  fixed_point_t x;
  x.f = f;
  return x;
}

/* Returns fixed-point number corresponding to integer N. */
static inline fixed_point_t
fix_int (int n)
{
  ASSERT (n >= FIX_MIN_INT && n <= FIX_MAX_INT);
  return __mk_fix (n * FIX_F);
}

/* Returns fixed-point number corresponding to N divided by D. */
static inline fixed_point_t
fix_frac (int n, int d)
{
  ASSERT (d != 0);
  ASSERT (n / d >= FIX_MIN_INT && n / d <= FIX_MAX_INT);
  return __mk_fix ((long long) n * FIX_F / d);
}

/* Returns X rounded to the nearest integer. */
static inline int
fix_round (fixed_point_t x)
{
  return (x.f + FIX_F / 2) / FIX_F;
}

/* Returns X truncated down to the nearest integer. */
static inline int
fix_trunc (fixed_point_t x)
{
  return x.f / FIX_F;
}

/* Returns X + Y. */
static inline fixed_point_t
fix_add (fixed_point_t x, fixed_point_t y)
{
  return __mk_fix (x.f + y.f);
}

/* Returns X - Y. */
static inline fixed_point_t
fix_sub (fixed_point_t x, fixed_point_t y)
{
  return __mk_fix (x.f - y.f);
}

/* Returns X * Y. */
static inline fixed_point_t
fix_mul (fixed_point_t x, fixed_point_t y)
{
  return __mk_fix ((long long) x.f * y.f / FIX_F);
}

/* Returns X * N. */
static inline fixed_point_t
fix_scale (fixed_point_t x, int n)
{
  ASSERT (n >= 0);
  return __mk_fix (x.f * n);
}

/* Returns X / Y. */
static inline fixed_point_t
fix_div (fixed_point_t x, fixed_point_t y)
{
  return __mk_fix ((long long) x.f * FIX_F / y.f);
}

/* Returns X / N. */
static inline fixed_point_t
fix_unscale (fixed_point_t x, int n)
{
  ASSERT (n > 0);
  return __mk_fix (x.f / n);
}

/* Returns 1 / X. */
static inline fixed_point_t
fix_inv (fixed_point_t x)
{
  return fix_div (fix_int (1), x);
}

/* Returns -1 if X < Y, 0 if X == Y, 1 if X > Y. */
static inline int
fix_compare (fixed_point_t x, fixed_point_t y)
{
  return x.f < y.f ? -1 : x.f > y.f;
}

#endif /* threads/fixed-point.h */
