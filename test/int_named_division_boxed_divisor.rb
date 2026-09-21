# Integer's named divisions with a divisor whose kind is only known at run
# time. `modulo` did not compile at all: the divisor went to sp_imod as a raw
# sp_RbVal, so a program containing the line could not be built. It converts
# the boxed value now, the way its sibling `remainder` already did.
b = [6, "x"].first          # a boxed Integer
p 17.modulo(b)
p 17.remainder(b)
p 17.div(b)
p 17 % b
p 17 / b
p(-17.modulo(b))
p(-17.remainder(b))

z = [0, "x"].first
begin
  p 17.modulo(z)
rescue ZeroDivisionError => e
  puts "ZeroDivisionError: #{e.message}"
end

# KNOWN DIVERGENCE, pinned so it is not mistaken for correct. A boxed Float
# divisor makes CRuby's answer a Float -- 17.remainder(2.5) is 2.0, 17.div(2.5)
# is 6 -- but these calls are typed Integer, so the divisor is forced to an
# integer first and the answer is a different number. The operators `%` and
# `/` get it right because they dispatch on the runtime kind instead.
#
# This predates the modulo fix above: `remainder` and `div` have always
# answered this way, and `modulo` merely joins them rather than staying
# unbuildable. Answering it properly means typing these calls poly, which the
# return-type derivation will not widen to (g_ret_no_new_poly) -- the #2024
# boundary. Until then the operators are the spelling that is right.
f = [2.5, "x"].first
puts "modulo    #{17.modulo(f)}   (CRuby 2.0)"
puts "remainder #{17.remainder(f)}   (CRuby 2.0)"
puts "div       #{17.div(f)}   (CRuby 6)"
puts "pct       #{17 % f}   (CRuby 2.0, and right)"
puts "/         #{17 / f}   (CRuby 6.8, and right)"
