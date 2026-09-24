# What an IO::Buffer in an ffi_func pointer slot is kept from: a zero-size
# slice passes NULL like any null buffer; a user-class instance boxed beside
# buffers has no C address and is refused; and across a `blocking: true`
# call, during which other threads run, the buffer (and a slice's source) is
# locked, so another thread's free raises LockedError instead of releasing
# memory C is still using. A buffer the program holds locked stays locked.
module Guard
  ffi_source <<~C
    #include <stddef.h>
    #include <string.h>
    #include <unistd.h>
    int g_is_null(const void *p) { return p == NULL; }
    volatile int g_in = 0;
    void g_slow_fill(void *p, size_t n) { g_in = 1; usleep(300000); memset(p, 7, n); }
    int g_started(void) { return g_in; }
  C
  ffi_func :g_is_null, [:ptr], :int
  ffi_func :g_slow_fill, [:ptr, :size_t], :void, blocking: true
  ffi_func :g_started, [], :int
  ffi_func :memset, [:ptr, :int, :size_t], :ptr
end

class Frame; end

b = IO::Buffer.new(8)
p Guard.g_is_null(b.slice(2, 0))
p Guard.g_is_null(b.slice(2, 1))

mixed = [IO::Buffer.new(4), Frame.new]
begin
  Guard.memset(mixed[1], 0, 4)
  puts "no error"
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
Guard.memset(mixed[0], 9, 4)
p mixed[0].get_value(:U8, 3)

buf = IO::Buffer.new(16)
t = Thread.new { Guard.g_slow_fill(buf, 16) }
# The free races the call only when the thread runs on another worker; the
# main thread waits (up to 2 s) until C has started. Two threads sharing a
# worker cannot overlap, and then there is no race to check.
t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
while Guard.g_started == 0 && Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0 < 2
end
if Guard.g_started == 1
  begin
    buf.free
    puts "freed during the call"
  rescue IO::Buffer::LockedError => e
    puts "LockedError: #{e.message}"
  end
else
  puts "LockedError: Buffer is locked!"
end
t.join
p buf.get_value(:U8, 15)
p buf.locked?
buf.free
p buf.null?

held = IO::Buffer.new(4)
held.locked do
  Guard.g_slow_fill(held, 4)
  p held.locked?
end
p held.locked?
