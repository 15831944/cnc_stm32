#include "miniutils.h"
#include "uart.h"

#define PUTC(p, c)  \
  if ((int)(p) < 256) \
    UART_put_char(_UART((int)(p)), (c)); \
  else \
    *((char*)(p)++) = (c);
#define PUTB(p, b, l)  \
  if ((int)(p) < 256) \
    UART_put_buf(_UART((int)(p)), (u8_t*)(b), (int)(l)); \
  else { \
    int ____l = (l); \
    memcpy((char*)(p),(b),____l); \
    (p)+=____l; \
  }

void v_printf(long p, const char* f, va_list arg_p) {
  register const char* tmp_f = f;
  register const char* start_f = f;
  char c;
  int format = 0;
  int num = 0;
  char buf[36];
  int flags = ITOA_FILL_SPACE;

  while ((c = *tmp_f++) != 0) {
    if (format) {
      switch (c) {
      case '%': {
        PUTC(p, '%');
        break;
      }
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        if (c == '0' && num == 0) {
          flags &= ~ITOA_FILL_SPACE;
        }
        num = num * 10 + (c - '0');
        num = MIN(sizeof(buf)-1, num);
        continue;
      case '+':
        flags |= ITOA_FORCE_SIGN;
        continue;
      case '#':
        flags |= ITOA_BASE_SIG;
        continue;
      case 'd':
      case 'i': {
        int v = va_arg(arg_p, int);
        if (v < 0) {
          v = -v;
          flags |= ITOA_NEGATE;
        }
        u_itoa(v, &buf[0], 10, num, flags);
        PUTB(p, &buf[0], strlen(&buf[0]));
        break;
      }
      case 'u': {
        u_itoa(va_arg(arg_p, unsigned int), &buf[0], 10, num, flags);
        PUTB(p, &buf[0], strlen(&buf[0]));
        break;
      }
      case 'p': {
        u_itoa(va_arg(arg_p, int), &buf[0], 16, 8, flags);
        PUTB(p, &buf[0], strlen(&buf[0]));
        break;
      }
      case 'X':
        flags |= ITOA_CAPITALS;
        // fall through
        //no break
      case 'x': {
        u_itoa(va_arg(arg_p, int), &buf[0], 16, num, flags);
        PUTB(p, &buf[0], strlen(&buf[0]));
        break;
      }
      case 'o': {
        u_itoa(va_arg(arg_p, int), &buf[0], 8, num, flags);
        PUTB(p, &buf[0], strlen(&buf[0]));
        break;
      }
      case 'b': {
        u_itoa(va_arg(arg_p, int), &buf[0], 2, num, flags);
        PUTB(p, &buf[0], strlen(&buf[0]));
        break;
      }
      case 'c': {
        int d = va_arg(arg_p, int);
        PUTC(p, d);
        break;
      }
      case 's': {
        char *s = va_arg(arg_p, char*);
        PUTB(p, s, strlen(s));
        break;
      }
      default:
        PUTC(p, '?');
        break;
      }
      start_f = tmp_f;
      format = 0;
    } else {
      if (c == '%') {
        if (tmp_f > start_f + 1) {
          PUTB(p, start_f, (int)(tmp_f - start_f - 1));
        }
        num = 0;
        format = 1;
        flags = ITOA_FILL_SPACE;
      }
    }
  }
  if (tmp_f > start_f + 1) {
    PUTB(p, start_f, (int)(tmp_f - start_f - 1));
  }
}

void uprint(int uart, const char* f, ...) {
  if (uart < 0 || uart >= CONFIG_UART_CNT) return;
  va_list arg_p;
  va_start(arg_p, f);
  v_printf(uart, f, arg_p);
  va_end(arg_p);
}

void print(const char* f, ...) {
  va_list arg_p;
  va_start(arg_p, f);
  v_printf(STDOUT, f, arg_p);
  va_end(arg_p);
}

void printbuf(u8_t *buf, u16_t len) {
  int i = 0, ix = 0;
  while (i < len) {
    for (i = ix; i < MIN(ix+32, len); i++) {
      print("%02x ", buf[i]);
    }
    while (i++ < ix+32) {
      print("   ");
    }
    print ("  ");
    for (i = ix; i < MIN(ix+32, len); i++) {
      print("%c", buf[i] < 32 ? '.' : buf[i]);
    }
    ix += 32;
    print("\n");
  }
}

void vprint(const char* f, va_list arg_p) {
  v_printf(STDOUT, f, arg_p);
}

void vuprint(int uart, const char* f, va_list arg_p) {
  if (uart < 0 || uart > CONFIG_UART_CNT) return;
  v_printf(uart, f, arg_p);
}

void sprint(char *s, const char* f, ...) {
  va_list arg_p;
  va_start(arg_p, f);
  v_printf((long)s, f, arg_p);
  va_end(arg_p);
}

static const char *I_BASE_ARR_L = "0123456789abcdefghijklmnopqrstuvwxyz";
static const char *I_BASE_ARR_U = "0123456789ABCDEFGHIJKLMNOPQRTSUVWXYZ";

void u_itoa(unsigned int v, char* dst, int base, int num, int flags) {
  // check that the base if valid
  if (base < 2 || base > 36) {
    if ((flags & ITOA_NO_ZERO_END) == 0) {
      *dst = '\0';
    }
    return;
  }

  const char *arr = (flags & ITOA_CAPITALS) ? I_BASE_ARR_U : I_BASE_ARR_L;
  char* ptr = dst, *ptr_o = dst, tmp_char;
  int tmp_value;
  int ix = 0;
  char zero_char = flags & ITOA_FILL_SPACE ? ' ' : '0';
  do {
    tmp_value = v;
    v /= base;
    if (tmp_value != 0 || (tmp_value == 0 && ix == 0)) {
      *ptr++ = arr[(tmp_value - v * base)];
    } else {
      *ptr++ = zero_char;
    }
    ix++;
  } while ((v && num == 0) || (num != 0 && ix < num));

  if (flags & ITOA_BASE_SIG) {
    if (base == 16) {
      *ptr++ = 'x';
      *ptr++ = '0';
    } else if (base == 8) {
      *ptr++ = '0';
    } else if (base == 2) {
      *ptr++ = 'b';
    }
  }

  // apply sign
  if (flags & ITOA_NEGATE) {
    *ptr++ = '-';
  } else if (flags & ITOA_FORCE_SIGN) {
    *ptr++ = '+';
  }

  // end
  if (flags & ITOA_NO_ZERO_END) {
    ptr--;
  } else {
    *ptr-- = '\0';
  }
  while (ptr_o < ptr) {
    tmp_char = *ptr;
    *ptr-- = *ptr_o;
    *ptr_o++ = tmp_char;
  }
}

void itoa(int v, char* dst, int base) {
  if (base == 10) {
    if (v < 0) {
      u_itoa(-v, dst, base, 0, ITOA_NEGATE);
    } else {
      u_itoa(v, dst, base, 0, 0);
    }
  } else {
    u_itoa((unsigned int) v, dst, base, 0, 0);
  }
}

void itoan(int v, char* dst, int base, int num) {
  if (base == 10) {
    if (v < 0) {
      u_itoa(-v, dst, base, num, ITOA_NEGATE | ITOA_NO_ZERO_END);
    } else {
      u_itoa(v, dst, base, num, ITOA_NO_ZERO_END);
    }
  } else {
    u_itoa((unsigned int) v, dst, base, num, ITOA_NO_ZERO_END);
  }
}

int atoin(const char* s, int base, int len) {
  int val = 0;
  char negate = FALSE;
  if (*s == '-') {
    negate = TRUE;
    s++;
    len--;
  } else if (*s == '+') {
    s++;
    len--;
  }
  while (len-- > 0) {
    int b = -1;
    char c = *s++;
    if (c >= '0' && c <= '9') {
      b = c - '0';
    } else if (c >= 'a' && c <= 'z') {
      b = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
      b = c - 'A' + 10;
    }
    if (b >= base) {
      return 0;
    }
    val = val * base + b;
  }
  return negate ? -val : val;
}

int strlen(const char* c) {
  const char *s;
  for (s = c; *s; ++s)
    ;
  return (int) (s - c);
}

int strnlen(const char *c, int size) {
  const char *s;
  for (s = c; size > 0 && *s; ++s, --size)
    ;
  return size >= 0 ? (int) (s - c) : 0;
}

int strcmp(const char* s1, const char* s2) {
  char c1, c2;
  while (((c1 = *s1++) != 0) & ((c2 = *s2++) != 0)) {
    if (c1 != c2) {
      return -1;
    }
  }
  return c1 - c2;
}

int strncmp(const char* s1, const char* s2, int len) {
  while (len > 0 && *s1++ == *s2++) {
    len--;
  }
  return len;
}

char* strncpy(char* d, const char* s, int num) {
  char* oldd = d;
  char c;
  while (num > 0 && (c = *s++) != 0) {
    *d++ = c;
    num--;
  }
  while (num-- > 0) {
    *d++ = 0;
  }
  return oldd;
}

char* strcpy(char* d, const char* s) {
  char* oldd = d;
  char c;
  do {
    c = *s++;
    *d++ = c;
  } while (c != 0);
  return oldd;
}

const char* strchr(const char* str, int ch) {
  char d;
  while ((d = *str) != 0 && d != ch) {
    str++;
  }
  if (d == 0) {
    return 0;
  } else {
    return str;
  }
}

char* strpbrk(const char* str, const char* key) {
  char c;
  while ((c = *str++) != 0) {
    if (strchr(key, c)) {
      return (char*)--str;
    }
  }
  return 0;
}

char* strnpbrk(const char* str, const char* key, int len) {
  char c;
  while (len-- > 0) {
    c = *str++;
    if (strchr(key, c)) {
      return (char*)--str;
    }
  }
  return 0;
}

char* strstr(const char* str1, const char* str2) {
  int len = strlen(str2);
  int ix = 0;
  char c;
  while ((c = *str1++) != 0 && ix < len) {
    if (c == str2[ix]) {
      ix++;
    } else {
      ix = 0;
    }
  }
  if (ix == len) {
    return (char*)(str1 - len);
  } else {
    return 0;
  }
}

char* strncontainex(const char* s, const char* content, int len) {
  while (len-- > 0) {
    if (strchr(content, *s++) == 0) {
      return (char*)--s;
    }
  }
  return 0;
}

unsigned short crc_ccitt_16(unsigned short crc, unsigned char data) {
  crc  = (unsigned char)(crc >> 8) | (crc << 8);
  crc ^= data;
  crc ^= (unsigned char)(crc & 0xff) >> 4;
  crc ^= (crc << 8) << 4;
  crc ^= ((crc & 0xff) << 4) << 1;
  return crc;
}

void quicksort(int* orders, void** pp, int elements) {

#define  MAX_LEVELS  32

  int piv;
  int beg[MAX_LEVELS];
  int end[MAX_LEVELS];
  int i = 0;
  int L, R, swap;

  beg[0] = 0;
  end[0] = elements;

  while (i >= 0) {
    void* pivEl;
    L = beg[i];
    R = end[i] - 1;

    if (L < R) {
      pivEl = pp[L];
      piv = orders[L];

      while (L < R) {
        while (orders[R] >= piv && L < R) {
          R--;
        }
        if (L < R) {
          pp[L] = pp[R];
          orders[L++] = orders[R];
        }
        while (orders[L] <= piv && L < R) {
          L++;
        }
        if (L < R) {
          pp[R] = pp[L];
          orders[R--] = orders[L];
        }
      }

      pp[L] = pivEl;
      orders[L] = piv;
      beg[i + 1] = L + 1;
      end[i + 1] = end[i];
      end[i++] = L;

      if (end[i] - beg[i] > end[i - 1] - beg[i - 1]) {
        swap = beg[i];
        beg[i] = beg[i - 1];
        beg[i - 1] = swap;

        swap = end[i];
        end[i] = end[i - 1];
        end[i - 1] = swap;
      }
    } else {
      i--;
    }
  }
}

void quicksortCmp(int* orders, void** pp, int elements,
    int(*orderfunc)(void* p)) {
  int i;
  for (i = 0; i < elements; i++) {
    orders[i] = orderfunc(pp[i]);
  }
  quicksort(orders, pp, elements);
}

void c_next(cursor *c) {
  if (c->len > 0) {
    c->len--;
    c->s++;
  }
}

void c_advance(cursor *c, int l) {
  l = MIN(l, c->len);
  c->len -= l;
  c->s += l;
}

void c_back(cursor *c) {
  c->len++;
  c->s--;
}

void c_skip_blanks(cursor *curs) {
  char c;
  while (curs->len > 0) {
    c = *(curs->s);
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      break;
    }
    c_next(curs);
  }
}

void c_strnparse(cursor *c, char str_def) {
  typedef enum {
    STR, BS_FIRST, BS_OCT, BS_HEX
  } pstate;
  pstate state = STR;
  int val = -1;
  while (c->len > 0) {
    char h = *c->s;

    switch (state) {
    case STR: {
      if (h == '\\') {
        state = BS_FIRST;
      } else if (h == str_def) {
        c_next(c);
        return;
      } else {
        *c->wrk++ = h;
      }
      c_next(c);
      break;
    } // case SR

    case BS_FIRST: {
      char advance = TRUE;
      // first backslash char
      switch (h) {
      case 'a':
        *c->wrk++ = '\a';
        state = STR;
        break;
      case 'b':
        *c->wrk++ = '\b';
        state = STR;
        break;
      case 'f':
        *c->wrk++ = '\f';
        state = STR;
        break;
      case 'n':
        *c->wrk++ = '\n';
        state = STR;
        break;
      case 'r':
        *c->wrk++ = '\r';
        state = STR;
        break;
      case 't':
        *c->wrk++ = '\t';
        state = STR;
        break;
      case 'v':
        *c->wrk++ = '\v';
        state = STR;
        break;
      case '\\':
      case '?':
      case '\'':
      case '\"':
        *c->wrk++ = h;
        state = STR;
        break;
      case 'x':
      case 'X':
        val = 0;
        state = BS_HEX;
        break;
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
        advance = FALSE;
        val = 0;
        state = BS_OCT;
        break;
      default:
        advance = FALSE;
        state = STR;
        break;
      }
      if (advance) {
        c_next(c);
      }
      break;
    } // case BS_FIRST

    case BS_OCT: {
      if (h >= '0' && h <= '7') {
        val = val * 8 + (h-'0');
        c_next(c);
      } else {
        *c->wrk++ = val;
        state = STR;
      }
      break;
    } // case BS_OCT

    case BS_HEX: {
      if (h >= 'A' && h <= 'F') {
        h = h - 'A' + 'a';
      }
      if (h >= '0' && h <= '9') {
        val = val * 16 + (h-'0');
        c_next(c);
      } else if ( h >= 'a' && h <= 'f') {
        val = val * 16 + (h-'a') + 10;
        c_next(c);
      } else {
        *c->wrk++ = val;
        state = STR;
      }
      break;
    } // case BS_HEX
    } // switch state
  } // while
  if (state == BS_OCT || state == BS_HEX) {
    *c->wrk++ = val;
  }
}

int strarg_next(cursor *c, strarg* arg) {
  c_skip_blanks(c);
  if (c->len == 0) {
    // end of string
    return FALSE;
  }

  // find starting string definition
  char str_def = 0;
  if (*c->s == '"') {
    str_def = '"';
  } else if (*c->s == '\'') {
    str_def = '\'';
  }

  // is a defined string, handle further
  if (str_def) {
    arg->type = STR;
    arg->str = c->wrk;
    // skip str def char
    c_next(c);
    // fill up c->wrk buffer with string parsed string
    c_strnparse(c, str_def);
    // and zero end
    *c->wrk++ = '\0';
    // calc len
    arg->len = c->wrk - arg->str;
    return TRUE;
  }

  // no defined string, find arg end ptr
  char *arg_end = strnpbrk(c->s, " \n\r\t", c->len);
  // adjust arg end if bad
  if (arg_end == 0 || arg_end - c->s > c->len) {
    arg_end = c->s + c->len;
  }

  arg->len = arg_end - c->s;

  // determine type

  // signs
  char possibly_minus = FALSE;
  char possibly_plus = FALSE;
  if (arg->len > 1) {
    possibly_minus = *c->s == '-';
    possibly_plus = *c->s == '+';
  }
  if (possibly_minus | possibly_plus) {
    c_next(c);
    arg->len--;
  }

  // content
  char possibly_hex = FALSE;
  char possibly_int = FALSE;
  char possibly_bin = FALSE;
  if (arg->len > 2 && *c->s == '0') {
    possibly_hex = ((*(c->s+1) == 'x' || *(c->s+1) == 'X'));
    possibly_bin = ((*(c->s+1) == 'b' || *(c->s+1) == 'B'));
  }

  if (possibly_bin) {
    possibly_bin = strncontainex(c->s + 2, "01", arg->len-2) == 0;
  }
  if (!possibly_bin && possibly_hex) {
    possibly_hex = strncontainex(c->s + 2, "0123456789abcdefABCDEF", arg->len-2) == 0;
  }
  if (!possibly_bin && !possibly_hex) {
    possibly_int = strncontainex(c->s, "0123456789", arg->len) == 0;
  }

  // ok, determined
  if (possibly_bin) {
    c_advance(c,2); // adjust for 0b
    arg->type = INT;
    arg->val = possibly_minus ? -atoin(c->s, 2, arg->len-2) : atoin(c->s, 2, arg->len-2);
    c_advance(c, arg->len - 2);
  } else if (possibly_int) {
    arg->type = INT;
    arg->val = possibly_minus ? -atoin(c->s, 10, arg->len) : atoin(c->s, 10, arg->len);
    c_advance(c, arg->len);
  } else if (possibly_hex) {
    c_advance(c,2); // adjust for 0x
    arg->type = INT;
    arg->val = possibly_minus ? -atoin(c->s, 16, arg->len-2) : atoin(c->s, 16, arg->len-2);
    c_advance(c, arg->len - 2);
  } else {
    arg->type = STR;
    arg->str = c->wrk;
    if (possibly_minus || possibly_plus) {
      c_back(c);
      arg->len++;
    }
    strncpy(arg->str, c->s, arg->len);
    arg->str[arg->len] = 0;
    c_advance(c, arg->len + 1);
    c->wrk += arg->len + 1;
  }

  return TRUE;
}

void strarg_init(cursor *c, char* s, int len) {
  c->s = s;
  c->wrk = s;
  if (len) {
    c->len = len;
  } else {
    c->len = strlen(s);
  }
}

#define ACC   (16)
#define PI    (1 << 9)
#define _B    ((4 << ACC) / PI)
#define _C    (-(4 << ACC) / (PI * PI))
inline short calc_sin(int a) {
  int sin = a * _B + _C * a * (a < 0 ? -a : a);
  return sin >> 1;
}
