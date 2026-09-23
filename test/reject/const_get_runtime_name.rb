# const_get with a name known only at run time is refused where it is
# written; it typed nothing and failed at run time (#4843).
module Carts
  class A
    def initialize(x) = @x = x
    def x = @x
  end
  TYPES = { 0 => :A }.freeze
  def self.runtime(t, x) = Carts.const_get(TYPES.fetch(t)).new(x)
end
p Carts.runtime(0, 5).x
