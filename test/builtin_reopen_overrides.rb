# A program's own reopen of a builtin primitive owns the name, as it does in
# CRuby: `class Integer; def abs; 999; end; end` makes `(-5).abs` answer 999.
# The reopened method was emitted and, for a name the compiler has no arm of
# its own for, called -- but an OVERRIDE of a real builtin method was
# silently ignored for a concrete receiver, because the dispatch that calls
# it sat after every builtin arm. Only a receiver typed at run time honoured
# a reopen.
class Integer
  def abs = 999
  def times = "no loop"
  def +(o) = 42
  def to_s(base = 10) = "INT"
  def succ = 77
  def shout = "int!"
end

class String
  def upcase = "nope"
  def to_s = "STR"
  def shout = "str!"
end

class Float
  def round(n = 0) = 3.5
end

p((-5).abs)
p 5.times
p(5 + 3)
p 5.to_s
p 5.succ
p 5.shout
p "ab".upcase
p "ab".to_s
p "ab".shout
p 1.234.round(1)

# A receiver typed only at run time honours a reopen where the poly dispatch
# has an arm for the name (abs, round). It does not yet for every name -- a
# poly `succ` still answers the builtin -- which is the other half of this
# gap and is not fixed here.
def pick(v) = v
p pick(-5).abs
p pick(1.234).round(1)

# the builtin still answers a name the reopen does not define
p((-5).magnitude)
p "ab".downcase
p 5.zero?
