# A method captured with method(:x) and reached only out of a container gets no
# argument evidence: the rule that binds a Method call site's argument types to
# its target fires only when the receiver is statically a Method, and
# `exports.fetch(name).call(*args)` hands back a poly value. Those parameters
# were pinned to Integer, on the grounds that the bound-Method ABI passes
# sp_int arguments -- but the ABI describes each argument's kind and carries
# the poly ones boxed, so it never needed the guess, and the guess was wrong
# for every program that puts a Float in one.
def le(a, b) = (a <= b) ? 1 : 0
def le_export(a, b) = le(a, b)
def id_export(a) = a

def invoke(name, *args)
  exports = { "le" => method(:le_export), "id" => method(:id_export) }
  exports.fetch(name).call(*args)
end

nan = [0xffc00000].pack("V").unpack1("e")
p invoke("le", -0.0, nan)
p invoke("le", -0.0, 1.0)
p invoke("le", 1, 2)
p invoke("id", 2.5)
p invoke("id", "str")
p invoke("id", :sym)
p invoke("id", [1, 2])
p invoke("id", nil)

# a parameter nothing typed takes what it is given, as CRuby does
class Base
  def untyped(s) = 42
end
p [Base.new.method(:untyped)][0].call("hello")
p [Base.new.method(:untyped)][0].call(7)
