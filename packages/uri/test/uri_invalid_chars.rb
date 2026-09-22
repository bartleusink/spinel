# A URI carrying a character RFC 3986 excludes is rejected, the way CRuby's
# parser rejects it -- an app can be relying on that rescue rather than on its
# own validation. Every line here answers what CRuby answers.
require "uri"

def parse(s)
  URI.parse(s)
  "parsed"
rescue URI::InvalidURIError
  "raised"
end

# The shape that surfaced it: with a space admitted, "exa mple.com" reads as an
# ordinary off-site host.
p parse("http://exa mple.com/ ")
p parse("http://example.com/a b")
p parse("http://example.com/<x>")
p parse(%(http://example.com/"x"))
p parse("http://example.com/x\u0000")
p parse("http://example.com/x\u007f")

# ...and the valid ones still parse, escapes included.
p parse("https://example.com/a/b?c=d#e")
p parse("http://user:pw@example.com:8080/x")
p parse("https://example.com/path%20with%20escape")
p parse("/rooms/1")
p parse("")
