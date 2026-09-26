# `\p{...}` character properties. The engine used to refuse every one of them
# at compile time, so a program carrying one did not build at all -- and the
# property IS the predicate, so nothing on the author's side can rewrite it
# (#4143).
#
# The engine (mruby-regexp) builds a property as the class of its ranges, from
# tables generated from the Unicode 17.0.0 database (lib/regexp/re_prop.h).
# The POSIX-named ones are the types re_ctype.h already carries and take the
# path the brackets take.

# The shape this arrived as: once-campfire's String#all_emoji?, verbatim.
class String
  def all_emoji?
    self.match?(/\A(\p{Emoji_Presentation}|\p{Extended_Pictographic}|️)+\z/u)
  end
end

p "🔔".all_emoji?
p "🔔🎉".all_emoji?
p "hi".all_emoji?
p "🔔x".all_emoji?

# \p{Word}, which rails_autolink uses to trim trailing punctuation.
p "see http://x.test/a." .sub(/[^\p{Word}\/]+\z/, "")
p "abc_123".scan(/\p{Word}/).join

s = "Aa1 _-€é́あ🔔"

# Two-letter categories, and the one-letter form that stands for all of them.
p s.scan(/\p{Lu}/)
p s.scan(/\p{Ll}/).join
p s.scan(/\p{L}/).join
p s.scan(/\p{Nd}/)
p s.scan(/\p{Pc}/) + s.scan(/\p{Pd}/)
p s.scan(/\p{P}/)
p s.scan(/\p{Sc}/)
p s.scan(/\p{Zs}/)
p s.scan(/\p{Mn}/).length

# Negated, both spellings.
p s.scan(/\P{L}/).length
p s.scan(/[^\p{L}]/).length

# Inside a class, mixed with literals and POSIX brackets.
p s.scan(/[\p{Lu}\p{Nd}_]/)
p s.scan(/[\p{Alpha}[:digit:]]/).join
p s.scan(/[^\p{L}\p{N}]/).length
p s.scan(/[\P{L}]/).length

# The name is matched the way CRuby matches it: case, underscores, hyphens and
# spaces are all the same name.
p "é".match?(/\p{alpha}/)
p "É".match?(/\p{Upper}/)
p "🔔".match?(/\p{extended pictographic}/)
p "🔔".match?(/\p{EXTENDED_PICTOGRAPHIC}/)
p "🔔".match?(/\p{Extended-Pictographic}/)

# A POSIX-named property folds under /i exactly as its bracket does, and so
# does a category, as a class does: /\p{Ll}/i holds "É" too.
p "É".match?(/\p{Lower}/i)
p "É".match?(/[[:lower:]]/i)
p "É".match?(/\p{Ll}/)
p "É".match?(/\p{Ll}/i)
p "e".match?(/\p{Lu}/i)
p "e".match?(/\P{Lu}/i)
p "e".match?(/[\P{Lu}]/i)

# Quantified, anchored, alternated, and inside a group.
p "Hello".match?(/\A\p{Lu}\p{Ll}+\z/)
p "hello".match?(/\A\p{Lu}\p{Ll}+\z/)
p "a1".match?(/\A(\p{L}|\p{N})+\z/)
p "Wörld! 123".gsub(/\p{L}+/) { |w| w.upcase }
