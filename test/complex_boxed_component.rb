# Kernel#Complex with a boxed component: `(sp_float)` on an sp_RbVal is not a
# conversion, it is a C error, so a Complex built from a value that reached the
# call through a container refused to compile at all.
x = [3, nil][0]
y = [2.5, nil][0]
p Complex(x)
p Complex(x, x)
p Complex(y, x)
p Complex(x, y)
p Complex(3, y)
p Complex(y, 4)

# A boxed component carries its class at run time; the Float-classed flag must
# come from the value, not from the static type, or an integral Float read out
# of a container renders and answers #real as an Integer.
f = [3.0, nil][0]
p Complex(f)
p Complex(f).real.class
p Complex(f, f)
p Complex(x, f)
p Complex(x, f).imaginary.class
p Complex(f, x).real.class
p Complex(f, 4)
p Complex(3, f)

