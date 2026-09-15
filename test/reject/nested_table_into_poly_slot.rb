# A nested numeric table (Array[Array[Float]]) reaching a slot that holds any
# kind of value: it has no boxed form, and the value used to be dropped
# (the method answered nil). Refused at compile time (#4486).
module Model
  def self.run(flag)
    if flag
      run_bcf
    end
  end
  def self.run_bcf
    a = Array.new(3, 0.0)
    b = Array.new(3, 0.0)
    [a, b]
  end
end
p Model.run(true)
