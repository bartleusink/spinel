# A minimal BigDecimal for Spinel (#4881): an exact decimal, in pure Ruby.
#
# The value is sign * mantissa * 10 ** exponent, the mantissa an array of
# decimal digits (least significant first). Digits rather than one Integer:
# a mantissa outgrows a 64-bit Integer at 19 digits, and Spinel's Integer
# only grows past that under --int-overflow=promote; this works in every mode.
#
# Covered: BigDecimal(Integer), BigDecimal(String) (decimal and exponent
# forms), + - * / with a BigDecimal, Integer or Float on either side,
# comparison (<=>, ==, <, >, clamp through Comparable), -@, abs, zero?,
# negative?, positive?, to_f, to_i, to_s / inspect in CRuby's "0.xxxen"
# form, and Math.sqrt through to_f (a Float, as in CRuby).
#
# Division carries DIV_DIGITS significant digits, rounding half up (CRuby's
# default mode). CRuby picks the precision per operation from the operands'
# sizes; to_f of a result agrees with CRuby's to the Float's precision, not
# necessarily in the last decimal digit of to_s.
#
# Not yet: a Float argument to BigDecimal() (CRuby requires a precision for
# it), precision arguments, rounding modes and #round, #sqrt, NaN and
# Infinity, BigDecimal::ROUND_* and the mode methods. Without Infinity, a
# division by zero raises ZeroDivisionError where CRuby's default mode
# answers Infinity.
class BigDecimal < Numeric
  include Comparable

  DIV_DIGITS = 40

  # sign: -1, 0 or 1; digits: least significant first, no leading or
  # trailing zero (a zero is [] with sign 0)
  def initialize(sign, digits, exp)
    lo = 0
    lo += 1 while lo < digits.length && digits[lo] == 0
    hi = digits.length
    hi -= 1 while hi > lo && digits[hi - 1] == 0
    if lo >= hi
      @sign = 0
      @digits = []
      @exp = 0
    else
      @sign = sign
      @digits = digits[lo...hi]
      @exp = exp + lo
    end
  end

  def __sign = @sign
  def __digits = @digits
  def __exp = @exp

  # ---- magnitudes: arrays of decimal digits, least significant first ----

  def self.__cmp_mag(a, b)
    return a.length <=> b.length if a.length != b.length
    i = a.length - 1
    while i >= 0
      return a[i] <=> b[i] if a[i] != b[i]
      i -= 1
    end
    0
  end

  def self.__add_mag(a, b)
    r = []
    carry = 0
    i = 0
    n = a.length > b.length ? a.length : b.length
    while i < n
      s = (i < a.length ? a[i] : 0) + (i < b.length ? b[i] : 0) + carry
      r << s % 10
      carry = s / 10
      i += 1
    end
    r << carry if carry > 0
    r
  end

  # a - b for a >= b
  def self.__sub_mag(a, b)
    r = []
    borrow = 0
    i = 0
    while i < a.length
      d = a[i] - (i < b.length ? b[i] : 0) - borrow
      if d < 0
        d += 10
        borrow = 1
      else
        borrow = 0
      end
      r << d
      i += 1
    end
    r.pop while r.length > 0 && r[r.length - 1] == 0
    r
  end

  def self.__mul_mag(a, b)
    return [] if a.empty? || b.empty?
    r = Array.new(a.length + b.length, 0)
    i = 0
    while i < a.length
      carry = 0
      j = 0
      while j < b.length
        t = r[i + j] + a[i] * b[j] + carry
        r[i + j] = t % 10
        carry = t / 10
        j += 1
      end
      k = i + b.length
      while carry > 0
        t = r[k] + carry
        r[k] = t % 10
        carry = t / 10
        k += 1
      end
      i += 1
    end
    r.pop while r.length > 0 && r[r.length - 1] == 0
    r
  end

  # a times 10 ** k
  def self.__shift_mag(a, k)
    return a if a.empty? || k <= 0
    Array.new(k, 0) + a
  end

  # [quotient, remainder] of a / b (b nonzero), by long division
  def self.__divmod_mag(a, b)
    q = Array.new(a.length, 0)
    rem = []
    i = a.length - 1
    while i >= 0
      rem = [a[i]] + rem
      rem.pop while rem.length > 0 && rem[rem.length - 1] == 0
      d = 0
      while __cmp_mag(rem, b) >= 0
        rem = __sub_mag(rem, b)
        d += 1
      end
      q[i] = d
      i -= 1
    end
    q.pop while q.length > 0 && q[q.length - 1] == 0
    [q, rem]
  end

  # ---- conversion ----

  def self.__from(v)
    return v if v.is_a?(BigDecimal)
    return __parse(v.to_s) if v.is_a?(Integer) || v.is_a?(Float)
    raise TypeError, "#{v.class} can't be coerced into BigDecimal"
  end

  def self.__parse(s)
    str = s.strip.delete("_")
    m = /\A([-+]?)(\d*)(?:\.(\d*))?(?:[eE]([-+]?\d+))?\z/.match(str)
    raise ArgumentError, "invalid value for BigDecimal(): \"#{s}\"" unless m
    whole = m[2]
    frac = m[3] || ""
    raise ArgumentError, "invalid value for BigDecimal(): \"#{s}\"" if whole.empty? && frac.empty?
    all = whole + frac
    digits = []
    i = all.length - 1
    while i >= 0
      digits << all[i].to_i
      i -= 1
    end
    exp = (m[4] ? m[4].to_i : 0) - frac.length
    BigDecimal.new(m[1] == "-" ? -1 : 1, digits, exp)
  end

  def coerce(other) = [BigDecimal.__from(other), self]

  # ---- arithmetic ----

  # the signed sum of (s1, a, e) and (s2, b, e2)
  def self.__add(s1, a, e1, s2, b, e2)
    e = e1 < e2 ? e1 : e2
    x = __shift_mag(a, e1 - e)
    y = __shift_mag(b, e2 - e)
    return BigDecimal.new(s2, y, e) if s1 == 0
    return BigDecimal.new(s1, x, e) if s2 == 0
    return BigDecimal.new(s1, __add_mag(x, y), e) if s1 == s2
    c = __cmp_mag(x, y)
    return BigDecimal.new(0, [], 0) if c == 0
    c > 0 ? BigDecimal.new(s1, __sub_mag(x, y), e) : BigDecimal.new(s2, __sub_mag(y, x), e)
  end

  def +(other)
    o = BigDecimal.__from(other)
    BigDecimal.__add(@sign, @digits, @exp, o.__sign, o.__digits, o.__exp)
  end

  def -(other)
    o = BigDecimal.__from(other)
    BigDecimal.__add(@sign, @digits, @exp, -o.__sign, o.__digits, o.__exp)
  end

  def *(other)
    o = BigDecimal.__from(other)
    BigDecimal.new(@sign * o.__sign, BigDecimal.__mul_mag(@digits, o.__digits), @exp + o.__exp)
  end

  def /(other)
    o = BigDecimal.__from(other)
    raise ZeroDivisionError, "divided by 0" if o.__sign == 0
    return BigDecimal.new(0, [], 0) if @sign == 0
    b = o.__digits
    # scale the dividend so the quotient has DIV_DIGITS significant digits
    k = DIV_DIGITS + b.length - @digits.length
    k = 0 if k < 0
    qr = BigDecimal.__divmod_mag(BigDecimal.__shift_mag(@digits, k), b)
    q = qr[0]
    # round half up: the remainder doubled against the divisor
    q = BigDecimal.__add_mag(q, [1]) if BigDecimal.__cmp_mag(BigDecimal.__add_mag(qr[1], qr[1]), b) >= 0
    BigDecimal.new(@sign * o.__sign, q, @exp - o.__exp - k)
  end

  def -@ = BigDecimal.new(-@sign, @digits, @exp)
  def +@ = self
  def abs = BigDecimal.new(@sign == 0 ? 0 : 1, @digits, @exp)
  def zero? = @sign == 0
  def negative? = @sign < 0
  def positive? = @sign > 0

  def <=>(other)
    return nil unless other.is_a?(BigDecimal) || other.is_a?(Integer) || other.is_a?(Float)
    (self - BigDecimal.__from(other)).__sign
  end

  def ==(other)
    return false unless other.is_a?(BigDecimal) || other.is_a?(Integer) || other.is_a?(Float)
    (self <=> other) == 0
  end

  def __mantissa_s
    s = +""
    i = @digits.length - 1
    while i >= 0
      s << @digits[i].to_s
      i -= 1
    end
    s
  end

  def to_f
    return 0.0 if @sign == 0
    "#{@sign < 0 ? "-" : ""}#{__mantissa_s}e#{@exp}".to_f
  end

  def to_i
    return 0 if @sign == 0
    m = __mantissa_s
    n = @exp >= 0 ? (m + "0" * @exp).to_i : (m.length + @exp > 0 ? m[0, m.length + @exp].to_i : 0)
    @sign < 0 ? -n : n
  end

  def to_s
    return "0.0" if @sign == 0
    m = __mantissa_s
    "#{@sign < 0 ? "-" : ""}0.#{m}e#{m.length + @exp}"
  end

  def inspect = to_s
end

def BigDecimal(v)
  return BigDecimal.__parse(v) if v.is_a?(String)
  raise ArgumentError, "can't omit precision for a Float." if v.is_a?(Float)
  BigDecimal.__from(v)
end
