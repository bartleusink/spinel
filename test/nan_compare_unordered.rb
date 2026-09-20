# NaN is unordered: it is neither less than, equal to, nor greater than
# anything, itself included. A three-way compare cannot say that, so the boxed
# comparisons read (gt - lt) == 0 as "equal" and answered true for <= and >=,
# 0 for <=>, and picked a winner for max where CRuby raises.
nan = [0xffc00000].pack("V").unpack1("e")
vals = [nan, -0.0, 1.0, 2]
box = [vals, nil][0]
a = box[0]
c = box[2]
d = box[3]
p [a < c, a <= c, a > c, a >= c, a == c]
p [c < a, c <= a, c > a, c >= a, c == a]
p [d < a, d <= a, d > a, d >= a]
p [a < a, a <= a, a >= a, a == a]
p(a <=> c)
p(c <=> a)
begin
  p [a, c].max
rescue ArgumentError
  puts "max: ArgumentError"
end
