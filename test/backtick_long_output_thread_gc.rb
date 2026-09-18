# A backtick answers the child's WHOLE output: it was one fread of 4095 bytes.
p `seq 1 5000`.lines.size
p `printf 'a\\0b'`.bytes
p `exit 3`
p $?.to_i >> 8

# A backtick, a Kernel#system or a Process.waitpid2 in a green thread waited
# for the child in the kernel on its OS worker. A worker in a syscall never
# reaches a safepoint, so main's next collection waited for the child too
# (#4528). Main collects many times while the children run; the threads are
# still alive when it is done.
slow = Thread.new { `sleep 1; echo slow` }
sys = Thread.new { system("sleep 1") }
sleep 0.1
20.times { garbage = Array.new(100) { |i| "s#{i}" }; GC.start }
p slow.alive?, sys.alive?
p slow.value
p sys.value
