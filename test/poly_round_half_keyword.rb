# `round(half:)` on a BOXED receiver. The tie-break mode is a keyword, not a
# positional argument: read as one it reached the digits slot and raised "no
# implicit conversion of Hash into Integer", while the typed Float and Integer
# paths have honoured the mode all along. A value that went through a container
# has to answer what the same value answers when it did not.
x = [8388609.0, nil][0]
p x.round(half: :even)
y = [2.5, nil][0]
p y.round(half: :even)
p y.round(half: :up)
p y.round(half: :down)
p y.round(half: nil)
p y.round
z = [2.345, nil][0]
p z.round(2, half: :even)
p z.round(2, half: :up)
p z.round(2)
n = [-15.0, nil][0]
p n.round(-1, half: :even)
i = [25, nil][0]
p i.round(half: :even)
p i.round(-1, half: :even)

# a mode read out of a container is chosen at run time
m = [:even, nil][0]
p y.round(half: m)

# only #round takes a tie-break mode; the others reject the keyword as CRuby does
[:floor, :ceil, :truncate].each do |op|
  begin
    y.send(op, half: :even)
    puts "#{op}: no raise"
  rescue TypeError => e
    puts "#{op}: #{e.message}"
  end
end
begin
  y.round(half: :bogus)
rescue ArgumentError => e
  puts "bogus: #{e.message}"
end
