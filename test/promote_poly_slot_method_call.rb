# A bound Method read out of a POLY slot and called under
# --int-overflow=promote. Promote widens int locals and parameters to poly,
# so the call-site arguments are boxed sp_RbVals; the dispatch used to hand
# them raw to the Proc arm's sp_int[16] slots (the generated C did not
# compile), and its Method arm re-read them unhoisted with no side-channel
# publish. The arguments now hoist once, publish boxed, and the Method arm
# is gated on the bind-time poly-ABI stamp: a target whose C signature is
# not `sp_RbVal fn([self,] sp_RbVal...)` -- a Float parameter, a rest, a
# keyword -- raises NoMethodError (as the legacy sp_int gate does in raise
# mode) instead of reading the registers as garbage.
class K
  def add2(a, b) = a + b
  def sub1(x) = x - 1
  def zero = 41
  def fmul(x) = x * 1.5           # Float return: no cast for it in either ABI
  def flagb(x) = @f = x > 3       # bool return over a boxed parameter
  def rest_t(*a) = a.length       # rest: never castable
end

def top3(a, b, c) = a * 100 + b * 10 + c
def tsub(a, b) = a - b

def call2(slots, i, a0, a1)
  _ty, func = slots[i]
  func.call(a0, a1)
end

k = K.new
slots = [[1, k.method(:add2)], [2, k.method(:sub1)], [3, method(:top3)]]

# bound instance method, two poly args
p call2(slots, 0, 3, 4)
# through arithmetic that promote boxes
x = 20
p call2(slots, 0, x + 1, x * 2)

# one-arg and zero-arg shapes
_ty, f1 = slots[1]
p f1.call(10)
zf = k.method(:zero)
box = [zf]
p box[0].call

# a SELF-LESS target (top-level def) out of the same poly slot: its C
# signature has no leading self, which the recv_bound branch picks
_ty, ft = slots[2]
p ft.call(1, 2, 3)

# a Proc beside the Methods in the same shape still rides the Proc arm,
# reading its boxed arguments from the published side channel
pslots = [[1, ->(a, b) { a * b }]]
p call2(pslots, 0, 6, 7)

def expect_nome(label)
  yield
  puts "#{label}: no raise"
rescue NoMethodError
  puts "#{label}: NoMethodError"
end

# targets whose signature is NOT the poly ABI decline loudly
fm = [k.method(:fmul)]
expect_nome("float_param") { fm[0].call(2) }
rs = [k.method(:rest_t)]
expect_nome("rest") { rs[0].call(1, 2) }
# arity mismatch against the stamped fixed count
expect_nome("arity") { slots[0][1].call(1) }

# bm[i] and bm[a, b] (Proc#[]-style call on a boxed Method) ride the same
# stamp -- including a self-less target, whose argument used to shift into
# the self slot
sq = [k.method(:sub1)]
p sq[0][8]
tt = [method(:tsub)]
p tt[0][7, 8]
ad = [k.method(:add2)]
p ad[0][20, 1]

# a MIXED signature promote leaves behind -- a bool return over a boxed
# parameter -- rides the same stamp through its own ret-kind cast
fl = [k.method(:flagb)]
p fl[0][5]
p fl[0].call(1)

# to_proc goes through the generic trampoline's poly branch
p slots.map { |_t, m| m }.first.to_proc.call(30, 5)
# and a spread call through sp_poly_callable_spread
args = [9, 8]
p slots[0][1].call(*args)
