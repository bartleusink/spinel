# Enumerable, in Ruby, compiled by spinel with the program that uses it.
#
# Each method here is the one definition of that name: the compiler has no
# C emitter and no runtime arm for it. A program that mentions one of these
# names has this file spliced ahead of it (spinel_parse.c), and the analyzer
# rewrites every definition into a top-level function that takes the
# receiver as its first parameter, `self` and the receiverless calls in the
# body becoming that parameter (analyze_desugar.c, desugar_builtins). A call
# `recv.m(args) { }` on an Array, a Hash, a Range, an Enumerator, a class
# that includes Enumerable, or a value only known at run time is rewritten
# into `__enum_m(recv, args) { }`, which the inliner then specializes for
# the receiver's type at that call site, exactly as it does for a yielding
# method the program wrote itself. A method that is never called never
# reaches the generated C.
#
# Write these in the Ruby the compiler compiles: `each` and `yield`, plain
# locals, no reflection. A body that needs an operation Ruby cannot express
# is a case for a C emitter, not for an intrinsic.
#
# A method that answers an Enumerator when called without a block says so
# with `if block_given?` as its last statement, the block arm first: the
# analyzer types a call with a block from that arm and a call without one
# from the other, and the inliner keeps only the arm the call site takes.
module Enumerable
  def each_with_object(memo)
    if block_given?
      each { |x| yield x, memo }
      memo
    else
      map { |x| [x, memo] }.each
    end
  end

  def partition
    if block_given?
      yes = []
      no = []
      each { |x| (yield(x) ? yes : no) << x }
      [yes, no]
    else
      each
    end
  end

  def group_by
    if block_given?
      groups = {}
      each { |x| (groups[yield(x)] ||= []) << x }
      groups
    else
      each
    end
  end

  def min_by(n = nil)
    if block_given?
      if n
        raise ArgumentError, "negative size (#{n})" if n < 0
        sort_by { |x| yield x }.first(n)
      else
        best = first
        if best.nil?
          nil
        else
          best_key = yield best
          skip = true
          each do |x|
            if skip
              skip = false
            else
              key = yield x
              if (key <=> best_key) < 0
                best = x
                best_key = key
              end
            end
          end
          best
        end
      end
    else
      each
    end
  end

  def max_by(n = nil)
    if block_given?
      if n
        raise ArgumentError, "negative size (#{n})" if n < 0
        # descending by key, ties in encounter order: a stable ascending sort
        # of the reversed elements, read backwards
        to_a.reverse.sort_by { |x| yield x }.reverse.first(n)
      else
        best = first
        if best.nil?
          nil
        else
          best_key = yield best
          skip = true
          each do |x|
            if skip
              skip = false
            else
              key = yield x
              if (key <=> best_key) > 0
                best = x
                best_key = key
              end
            end
          end
          best
        end
      end
    else
      each
    end
  end

  def minmax_by
    if block_given?
      min = first
      if min.nil?
        [nil, nil]
      else
        max = min
        min_key = yield min
        max_key = min_key
        skip = true
        each do |x|
          if skip
            skip = false
          else
            key = yield x
            if (key <=> min_key) < 0
              min = x
              min_key = key
            end
            if (key <=> max_key) > 0
              max = x
              max_key = key
            end
          end
        end
        [min, max]
      end
    else
      each
    end
  end

  def filter_map
    if block_given?
      out = []
      each do |x|
        v = yield x
        out << v if v
      end
      out
    else
      each
    end
  end

  def flat_map
    if block_given?
      out = []
      each do |x|
        v = yield x
        if v.is_a?(Array)
          out.concat(v)
        else
          out << v
        end
      end
      out
    else
      each
    end
  end

  def take_while
    if block_given?
      out = []
      each do |x|
        break unless yield x
        out << x
      end
      out
    else
      each
    end
  end

  def drop_while
    if block_given?
      out = []
      dropping = true
      each do |x|
        dropping = false if dropping && !yield(x)
        out << x unless dropping
      end
      out
    else
      each
    end
  end
end
