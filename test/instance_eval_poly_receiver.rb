# instance_eval / instance_exec on a receiver whose static type is poly (a
# hash read, `x ||= Klass.new`): the block runs as whichever class the value
# holds at run time. Previously the forwarded form raised NoMethodError for
# instance_eval itself and the literal form was refused at compile time.

class Recorder
  attr_reader :ev
  def initialize = (@ev = [])
  def event(n) = (@ev << n; n)
end

H = {}

def dsl(&b) = (m = H[:x]; m ||= Recorder.new; m.instance_eval(&b); m)
def dsl_false(&b) = (m = false && H[:x]; m ||= Recorder.new; m.instance_eval(&b); m)
def dsl_keyed(&b) = (m = (H.key?(:x) && H[:x]) || nil; m ||= Recorder.new; m.instance_eval(&b); m)
def dsl_exec(&b) = (m = H[:x]; m ||= Recorder.new; m.instance_exec(&b); m)

p dsl { event :a }.ev
p dsl_false { event :b; event :c }.ev
p dsl_keyed { event :d }.ev
p dsl_exec { event :e }.ev

# the block's value
def value_of(&b) = (m = H[:x]; m ||= Recorder.new; m.instance_eval(&b))
p value_of { event :f }
p value_of { event(:g); 42 }
r = H.fetch(:y, nil) || Recorder.new
p r.instance_eval { event(:h).to_s + "!" }
p r.instance_eval { x = event(:i); @ev.size + x.size }

# two classes answering the DSL method differently, and a subclass
class Loud
  attr_reader :log
  def initialize = (@log = [])
  def say(w) = (@log << w.to_s.upcase; @log.size)
end

class Quiet
  attr_reader :log
  def initialize = (@log = [])
  def say(w) = (@log << w.to_s; -@log.size)
end

class Whisper < Quiet
end

REG = {}

def build(key, &blk)
  obj = REG[key]
  obj ||= case key
          when :loud then Loud.new
          when :quiet then Quiet.new
          else Whisper.new
          end
  REG[key] = obj
  obj.instance_eval(&blk)
  obj
end

p build(:loud) { say :hi; say :there }.log
p build(:quiet) { say :hi }.log
p build(:whisper) { say :psst }.log
p build(:loud) { say :again }.log

p REG[:loud].instance_eval { say(:x) }
p REG[:quiet].instance_eval { say(:x) }
p REG[:whisper].instance_eval { [say(:y), say(:z)] }
p REG[:loud].instance_eval { say(:c); self.class }
p REG[:quiet].instance_eval { [1, 2].map { |i| say(i) } }

# instance_exec with arguments on a poly receiver
p REG[:loud].instance_exec(:a, :b) { |x, y| say(x); say(y) }
p REG[:quiet].instance_exec(3) { |n| say(n * 2) }
def exec_on(key, *args, &blk) = REG[key].instance_exec(*args, &blk)
p exec_on(:whisper, 7) { |n| say(n + 1) }
p exec_on(:loud, 1, 2) { |a, b| say(a + b) }
p REG[:quiet].log

# a receiver that cannot run the block raises like CRuby
begin
  REG[:missing].instance_eval { say :nope }
rescue NoMethodError => e
  puts e.message
end
