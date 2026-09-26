# Methods a class gets through `extend M` run as class methods of that class:
# `self` is the class, and the block is the one the class method's caller
# passes. They used to share the module's body, typed against the module, so a
# block param, a yield, a forwarded `&` or a `self` handed out broke the C.

class K
  def self.name_of(o, n) = "#{o.name}:#{n}"
  def self.apply(o, n, &) = yield(o, n)
end

module MM
  def via_blk(n, &b) = b.call(n)
  def via_yield(n, &) = yield(n)
  def forward(n, &) = K.apply(self, n, &)
  def tag(n) = K.name_of(self, n)
  def scaled(n = 3, &b) = b.call(n) * 2
  def owner_name = describe(self)
  def describe(o) = "<#{o.name}>"
end

class V
  extend MM
  p(via_blk(:a) { |x| x.to_s })
  p(via_yield(:b) { |x| x.to_s })
  p(forward(:c) { |o, x| "#{o.name}/#{x}" })
  p tag(:d)
  p(scaled { |x| x + 1 })
end

class W
  extend MM
  p(scaled(10) { |x| x - 1 })
  p owner_name
end

p V.via_blk(1) { |x| x + 1 }
p W.via_yield("s") { |x| x * 2 }
p W.forward(2) { |o, x| [o.name, x] }
p V.tag(:e)
p V.owner_name

# The macro-module DSL shape: the class body calls a macro that hands the
# class and the block on to a builder.
module MacroMethods
  def machine(name = :state, &) = Machine.find_or_create(self, name, &)
end

module Registry
  def machines = @machines
end

class Machine
  attr_reader :events

  def self.find_or_create(owner, name, &)
    m = new(owner, name)
    m.instance_eval(&) if block_given?
    m
  end

  def initialize(owner, name)
    @events = []
    owner.machines[name] = self
  end

  def event(n) = @events << n
end

class Vehicle
  extend Registry
  @machines = {}
  extend MacroMethods
  machine :state do
    event :ignite
    event :park
  end
end

p Vehicle.machines[:state].events
