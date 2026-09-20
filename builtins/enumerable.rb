# Enumerable, in Ruby, compiled by spinel with the program that uses it.
#
# Each method here is the one definition of that name: the compiler has no
# C emitter and no runtime arm for it. A program that mentions one of these
# names has this file spliced ahead of it (spinel_parse.c), and the analyzer
# rewrites every definition into a top-level function that takes the
# receiver as its first parameter, `self` and the receiverless calls in the
# body becoming that parameter (analyze_desugar.c, desugar_builtins). A call
# `recv.m(args) { }` on an Array, a Hash, a Range, an Enumerator, a class
# that includes Enumerable, or a value only known at run time is rewritten
# into `__enum_m(recv, args) { }`, which the inliner then specializes for
# the receiver's type at that call site, exactly as it does for a yielding
# method the program wrote itself. A method that is never called never
# reaches the generated C.
#
# Write these in the Ruby the compiler compiles: `each` and `yield`, plain
# locals, no reflection. A body that needs an operation Ruby cannot express
# is a case for a C emitter, not for an intrinsic.
#
# A method that answers an Enumerator when called without a block says so
# with `if block_given?` as its last statement, the block arm first: the
# analyzer types a call with a block from that arm and a call without one
# from the other, and the inliner keeps only the arm the call site takes.
module Enumerable
  def each_with_object(memo)
    if block_given?
      each { |x| yield x, memo }
      memo
    else
      map { |x| [x, memo] }.each
    end
  end
end
