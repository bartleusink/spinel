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
