# A small method that reaches itself only through another method's parameter
# default: the default is emitted at the call site, so the method calls itself
# in C and must not be forced inline (gcc refuses a recursive always_inline).

def g(n) = n > 0 ? f(n - 1) : 0
def f(n, a = g(n)) = a + 1
p f(3)

def h(n) = n > 0 ? k(n - 1, 10) + k(n - 1) : 0
def k(n, a = h(n)) = a + 1
p h(4)
