# A stack the cache hands to a new thread keeps its guard page: a thread whose
# recursion runs off the end of a reused stack faults on the guard and gets
# its SystemStackError, which it can rescue, rather than writing into
# whatever lies below. The rounds first leave the cache full of stacks that
# earlier threads ran on.
ENV["SPINEL_FIBER_STACK_CACHE"] = "4"

def depth(n) = n == 0 ? 0 : 1 + depth(n - 1)
def deep(n) = n < 0 ? 0 : 1 + deep(n + 1)

20.times { Array.new(8) { |w| Thread.new(w) { |k| depth(50 + k) } }.each(&:join) }
puts "cache filled"

r = Array.new(3) do
  Thread.new do
    begin
      deep(0)
      :no
    rescue SystemStackError => e
      e.class
    end
  end
end.map(&:value)
p r

# and threads after them run on the cached stacks as before
p Array.new(8) { |w| Thread.new(w) { |k| depth(10 + k) } }.sum(&:value)
