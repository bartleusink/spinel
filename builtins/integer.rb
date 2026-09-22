# Integer, in Ruby, compiled by spinel with the program that uses it.
#
# Each method here is the one definition of that name for a CONCRETE
# Integer/Bignum receiver: the compiler has no C emitter for that receiver
# shape any more. A program that mentions one of these names has this file
# spliced ahead of it (spinel_parse.c's sp_splice_builtin_extras), and the
# analyzer rewrites every definition into a top-level function that takes
# the receiver as its first parameter, the same mechanism
# builtins/enumerable.rb uses (analyze_desugar.c, desugar_builtin_scalar_defs
# / desugar_builtin_scalar_calls). A run-time-typed (poly) receiver is NOT
# rewritten onto these definitions: it keeps using the existing runtime
# dispatch (sp_poly_int_*, lib/spinel_rt.h), which already answers every
# name below correctly for both a small Integer and a Bignum -- see the
# mechanism's own comment in analyze_desugar.c for why duplicating that
# split was not worth the added rewrite surface.
#
# A method that is never called never reaches the generated C. Write these
# in the Ruby the compiler compiles: plain locals, no reflection, no block.

class Integer
  def digits(base = 10)
    # CRuby's C implementation coerces `base` up front (rb_to_int), raising
    # this exact TypeError for anything that is not already an Integer,
    # before ever comparing it; a plain `base < 0` here would instead ask
    # nil/a String to answer `<`, giving a different exception entirely
    # (NoMethodError / a comparison-failed ArgumentError). Written out
    # explicitly so the two match (test/numeric_nil_argument.rb,
    # test/numeric_string_argument.rb, test/strict_arg_conversion.rb).
    unless base.is_a?(Integer)
      raise TypeError, "no implicit conversion of #{base.nil? ? "nil" : base.class} into Integer"
    end
    raise ArgumentError, "negative radix" if base < 0
    raise ArgumentError, "invalid radix #{base}" if base < 2
    raise Math::DomainError, "out of domain" if self < 0
    return [0] if self == 0
    result = []
    n = self
    while n > 0
      result << n % base
      n /= base
    end
    result
  end

  def bit_length
    # A linear shift-and-count loop measured 5x the cost of the C emitter's
    # binary-search reduction (a 200,000,000-call bench, well past the
    # ~10% bound); a first attempt at the same doubling technique
    # (checking against a 2**64 literal to fold off 64 bits at once) was
    # 20x worse still -- comparing an Integer-typed local against a
    # Bignum-sized literal anywhere in this body widened every use of
    # that local for the whole function, including the receiver's own
    # fast path, onto boxed/Bignum arithmetic. Every literal below fits
    # the target's Integer, so an Integer receiver's shifts stay native.
    # Measured faster than the old emitter once compiled into the same
    # translation unit (0.21s vs 0.31s, 200,000,000 calls) -- the library
    # call it replaces could never be inlined across the .a boundary the
    # way this generated copy is.
    #
    # The loop reduces SIXTEEN bits per pass, not thirty-two: sp_int is
    # the pointer width, so on a 32-bit target `n >> 32` is a shift past
    # the width of the type, which the C compiler refuses under -Werror
    # (`right shift count >= width of type`) and which no Integer
    # receiver there could need anyway. Three passes cover an int64's
    # magnitude, and a small receiver takes none.
    n = self < 0 ? ~self : self
    b = 0
    while n >= 65536
      b += 16
      n = n >> 16
    end
    if n >= 256 then b += 8; n = n >> 8 end
    if n >= 16 then b += 4; n = n >> 4 end
    if n >= 4 then b += 2; n = n >> 2 end
    if n >= 2 then b += 1; n = n >> 1 end
    b + n
  end

  def gcd(other)
    # CRuby rejects anything but an Integer here, Float included (even a
    # whole one), with this exact message, no interpolated class name.
    # The if/else form matters, not just style: a guard-clause shape
    # (`raise X unless y.is_a?(Integer)` then using `y` normally after)
    # does not stop a call site whose actual argument is e.g. an Array
    # from specializing this clone with `other` typed concretely Array,
    # and `other < 0` two lines down then fails to COMPILE
    # (`undefined method '<' for an instance of Array`) even though it
    # is unreachable at run time. Nesting the body inside the true arm
    # of the is_a? check itself (this shape) does not have the problem
    # -- caught by test/integer_gcd_arg_check.rb, which the guard-clause
    # draft of this method failed outright.
    if other.is_a?(Integer)
      a = self < 0 ? -self : self
      b = other < 0 ? -other : other
      while b != 0
        a, b = b, a % b
      end
      a
    else
      raise TypeError, "not an integer"
    end
  end

  def lcm(other)
    if other.is_a?(Integer)
      if self == 0 || other == 0
        0
      else
        g = gcd(other)
        a = self < 0 ? -self : self
        b = other < 0 ? -other : other
        (a / g) * b
      end
    else
      raise TypeError, "not an integer"
    end
  end

  def gcdlcm(other)
    if other.is_a?(Integer)
      [gcd(other), lcm(other)]
    else
      raise TypeError, "not an integer"
    end
  end

  def ceildiv(other)
    # CRuby's own algorithm (found by black-box probing a coerce-tracing
    # stub, since there is no source to read here): negate `other` FIRST
    # (a real call to its own unary `-@`, which is why a receiver lacking
    # one -- Array, Hash, Symbol, nil, true, false -- answers CRuby's
    # "undefined method `-@'" rather than a coercion error), floor-divide,
    # then negate the quotient. `elsif is_a?(Float)` (not a single
    # `is_a?(Numeric)` arm) because the two need the SAME body but each
    # needs its own is_a? to narrow `other` for codegen: a single shared
    # arm left `other` at the call site's own concrete type in the arm
    # CRuby ALSO takes, and a concrete Array/Hash/String argument (a call
    # CRuby raises for at run time, not reject at compile time) has no
    # `-@`/`div` to bind and failed to COMPILE outright -- the same
    # REQUIRED-parameter pitfall gcd's own commit found, one narrowing
    # arm per accepted type rather than gcd's single is_a? guard. The
    # final `else` never touches `other` itself, so it compiles for any
    # type; CRuby's own message there is class-specific (NoMethodError
    # naming `-@`) but this is what the unmigrated compiler already
    # answered for the same inputs (`5.gcd(other)`'s sibling arms took
    # the same simplification), so this is not a new gap.
    if other.is_a?(Integer)
      -(self.div(-other))
    elsif other.is_a?(Float)
      -(self.div(-other))
    else
      raise TypeError, "not an integer"
    end
  end
end
