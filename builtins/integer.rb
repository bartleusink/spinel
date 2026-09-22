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
end
