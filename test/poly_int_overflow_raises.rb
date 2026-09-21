# raise/wrap-mode only: integer arithmetic on a BOXED receiver overflows the
# machine word the same way the typed path does -- loudly.
#
# It used to wrap silently: `[2**62, nil][0] * 2` answered **nil**, because
# the wrapped low word is INTPTR_MIN and that is the nil sentinel, so the
# program failed later at something that never mentions arithmetic. `* 4`
# answered 0. The typed spelling of the same expression has always raised.
n = [2**62, nil][0]      # boxed, and small enough to be a tagged Integer
def chk(tag)
  v = yield
  puts "#{tag}: no raise -> #{v.inspect}"
rescue RangeError => e
  puts "#{tag}: #{e.message}"
end
chk("mul2")  { n * 2 }
chk("mul4")  { n * 4 }
chk("mulself") { n * n }
chk("add")   { n + n }
chk("sub")   { n - (-n) }

# the typed spelling answers identically -- that is the point
t = 2**62
chk("typed mul2") { t * 2 }
chk("typed add")  { t + t }

# arithmetic that does NOT overflow is untouched, boxed or typed
p n * 1, n + 0, n - 1, n / 2
p [3, nil][0] * 4, [3, nil][0] + 4, [3, nil][0] - 4
p 6 * 7
