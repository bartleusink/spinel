# Only the program's own classes get the methods it adds to Class: on a
# builtin class the call is refused where it is written, rather than raising
# the NoMethodError CRuby would not.
class Class
  def macro = :ok
end

p String.macro
