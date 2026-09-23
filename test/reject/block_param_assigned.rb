# A method's `&block` parameter is read-only in spinel.
#
# A method that yields is inlined at each call site, where its block is the
# caller's block CODE pasted in at the yields rather than a value, so a write
# to the parameter has no variable to land in -- `b = nil` emitted an
# assignment to an lv_b nothing declares and the C build stopped. Allowing
# the write only where the method does not yield would make an unrelated
# `b = ...` line stop compiling the day a yield is added elsewhere in the
# body, so the rule is one rule. The write is refused with the rewrite that
# does the same thing: `blk = b || proc { ... }`.
def m(&b)
  b = nil if ARGV.length > 5
  return 7 unless b
  yield 1
end
p m { |n| n + 1 }
