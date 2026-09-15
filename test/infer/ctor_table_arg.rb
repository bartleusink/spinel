# A table of int arrays built as a LOCAL in a `self.load` factory and handed to
# the constructor. Emission resolves `T.new(...)` to T#initialize, so the
# narrowing pass has to resolve it the same way. `new` names no class method --
# it is implicit -- and reading that lookup's -1 as "the callee cannot be
# attributed" killed the caller's slot, so the table stayed a boxed poly array
# and every helper it feeds bound a boxed parameter. The identical table
# assigned to the ivar inside `initialize` narrowed, which made the boxing
# depend only on WHERE the table was built.
class F
  def self.mul(a, b)
    (a * b) % 97
  end
end

class T
  attr_reader :t
  def initialize(rows)
    @t = rows
  end

  def total(s)
    acc = 0
    i = 0
    while i < @t.length
      row = @t[i]
      acc += F.mul(row[0], s)
      i += 1
    end
    acc
  end
end

def self.load(n)
  rows = Array.new(n) { |c| [c, c + 1, c + 2] }
  T.new(rows)
end

tb = load(4)
p tb.total(3)
p tb.t[2][1]
