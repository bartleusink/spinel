# A Float whose integer value escapes int64, on its way into a slot that holds
# sp_int. The bare cast is C UB, and not the harmless kind: it saturated at
# -O0 and, from -O1, the optimizer took the UB as a promise and deleted the
# statement around it -- `p (2.0**70).floor` printed NOTHING and exited 0.
# Every route raises now. CRuby answers the Bignum; that promotion is a
# separate matter, but a loud error is not an undefined value.
big = 2.0**70
def poly(x) = x

[:floor, :ceil, :round, :truncate, :to_i, :to_int].each do |m|
  begin
    big.send(m)
    puts "#{m}: NO RAISE"
  rescue => e
    puts "#{m}: #{e.class}"
  end
end
begin; big.floor;      rescue => e; puts "floor direct: #{e.class}"; end
begin; big.ceil;       rescue => e; puts "ceil direct: #{e.class}"; end
begin; big.round;      rescue => e; puts "round direct: #{e.class}"; end
begin; big.truncate;   rescue => e; puts "truncate direct: #{e.class}"; end
begin; big.to_i;       rescue => e; puts "to_i direct: #{e.class}"; end
begin; Integer(big);   rescue => e; puts "Integer(): #{e.class}"; end
begin; big.floor(-1);  rescue => e; puts "floor(-1): #{e.class}"; end
n = -1
begin; big.round(n);   rescue => e; puts "round(runtime -1): #{e.class}"; end
begin; big.div(1.0);   rescue => e; puts "div: #{e.class}"; end
begin; big.divmod(1.0); rescue => e; puts "divmod: #{e.class}"; end
begin; poly(big).floor;    rescue => e; puts "poly floor: #{e.class}"; end
begin; poly(big).ceil;     rescue => e; puts "poly ceil: #{e.class}"; end
begin; poly(big).round;    rescue => e; puts "poly round: #{e.class}"; end
begin; poly(big).truncate; rescue => e; puts "poly truncate: #{e.class}"; end
begin; poly(big).round(-1); rescue => e; puts "poly round(-1): #{e.class}"; end
begin; (-big).floor;   rescue => e; puts "negative floor: #{e.class}"; end

# a non-finite value is still FloatDomainError, named as CRuby names it
inf = 1.0 / 0.0
nan = inf - inf
begin; inf.floor;  rescue => e; puts "inf floor: #{e.class}: #{e.message}"; end
begin; (-inf).ceil; rescue => e; puts "-inf ceil: #{e.class}: #{e.message}"; end
begin; nan.round;  rescue => e; puts "nan round: #{e.class}: #{e.message}"; end
begin; poly(inf).floor; rescue => e; puts "poly inf floor: #{e.class}: #{e.message}"; end

# and nothing in range moved
p 3.7.floor, 3.2.ceil, 3.5.round, (-3.7).truncate, 3.7.to_i, Integer(3.9)
p (-3.7).floor, (-3.2).ceil, (-2.5).round, 2.5.round
p 1234.5678.floor(2), 1234.5678.round(-2), 1234.5678.ceil(-2), 1234.5678.truncate(-2)
d = -2
p 1234.5678.round(d)
p 7.5.div(2.0), 7.5.divmod(2.0)
p poly(3.9).floor, poly(3.1).ceil, poly(3.5).round, poly(-3.9).truncate, poly(1234.5678).round(-2)
# the largest Float that still fits, and the first that does not
p 9223372036854774784.0.to_i
begin; 9223372036854775808.0.to_i; rescue => e; puts "2**63: #{e.class}"; end
p(-9223372036854774784.0.to_i)
begin; (-9223372036854777856.0).to_i; rescue => e; puts "-2**63 - eps: #{e.class}"; end
