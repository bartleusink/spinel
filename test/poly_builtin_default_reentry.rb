# A builtin receiver reaching the poly method dispatch because a user class
# owns the name, for a name nothing wrote an arm for.
#
# The dispatch's default arm was a chain of hand-written cases, one per name,
# and a name nobody had added raised NoMethodError for a method the receiver
# has. The same gap was found and patched three times over the numeric names
# alone. The default now asks the ordinary call emission what a builtin
# receiver would answer -- the re-entry the container arm (#3459) and the
# String tag pre-arm (#4816) already use -- so a name no table carries gets
# whatever the surface serves.
#
# `Shadow` defines each name with two parameters and every call below passes
# none, so no user arm is a candidate and the answer has to come from the
# receiver itself.
class Shadow
  def all?(a, b) = :shadow
  def sum(a, b) = :shadow
  def to_sym(a, b) = :shadow
  def to_a(a, b) = :shadow
end

def pick(x) = x

s = Shadow.new
p s.all?(1, 2)
p s.sum(1, 2)
p s.to_sym(1, 2)
p s.to_a(1, 2)

p pick(1).class
p pick(7.5).class

p pick([3, 1, 2]).all?
p pick("abc").sum
p pick(:abc).to_sym
p pick([3, 1, 2]).to_a

# a receiver the surface cannot serve keeps its NoMethodError: the runtime
# helper the arm ends in asks the tag, and nothing else answers for it
class Bare; end
begin
  p pick(Bare.new).to_a
rescue NoMethodError => e
  puts "NoMethodError"
end
