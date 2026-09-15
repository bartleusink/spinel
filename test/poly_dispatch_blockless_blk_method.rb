# A method whose &blk is only called under `blk.nil?` inlines at its call
# sites, so nothing keeps a standalone entry when every caller is block-less.
# A call through a poly receiver dispatches by class to a symbol, so the
# candidate still needs one: the poly-receiver call site is a caller the
# inline pass cannot see (#4492). The yield shapes beside it cover the
# proc-form arm: a by-value receiver, and a blockless call of a yielding
# method, which is LocalJumpError.
class Req
  attr_accessor :body
end

class Http
  def initialize(host)
    @host = host
  end

  def request(req, &blk)
    res = "#{@host}:#{req.body}"
    blk.call(res) unless blk.nil?
    res
  end

  def each_line
    yield "l1"
    yield "l2"
  end
end

class Other
  def request(req)
    "other:#{req.body}"
  end

  def each_line
    yield "o"
  end
end

slots = { "h" => Http.new("example.test"), "o" => Other.new }
req = Req.new
req.body = "hello"
puts slots["h"].request(req)
puts slots["o"].request(req)
puts slots["h"].request(req) { |r| puts "cb #{r}" }
slots["h"].each_line { |l| puts l }
slots["o"].each_line { |l| puts l }
begin
  slots["h"].each_line
rescue LocalJumpError => e
  puts "LocalJumpError: #{e.message}"
end
h = Http.new("x")
puts h.request(req)
