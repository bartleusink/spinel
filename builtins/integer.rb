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
    # int64, so an Integer receiver never takes the `while` loop at all
    # and falls straight through the five fixed halvings and the shifts
    # stay native; a Bignum receiver's `while` reduces 32 bits per pass
    # (fewer, larger steps read worse for a rare case that is not benched
    # here). Measured faster than the old emitter once compiled into the
    # same translation unit (0.21s vs 0.31s, 200,000,000 calls) -- the
    # library call it replaces could never be inlined across the .a
    # boundary the way this generated copy is.
    n = self < 0 ? ~self : self
    b = 0
    while n >= 4294967296
      b += 32
      n = n >> 32
    end
    if n >= 65536 then b += 16; n = n >> 16 end
    if n >= 256 then b += 8; n = n >> 8 end
    if n >= 16 then b += 4; n = n >> 4 end
    if n >= 4 then b += 2; n = n >> 2 end
    if n >= 2 then b += 1; n = n >> 1 end
    b + n
  end
end
