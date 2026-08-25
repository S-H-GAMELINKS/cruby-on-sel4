# Interactive Ruby shell over the serial console, with Reline for line editing.
#
# The embedded CPIO archive is rooted at '/', and the file shims strip the leading
# slash before looking an entry up, so one load path entry covers everything.
$LOAD_PATH.unshift('/')

require 'reline'

puts "Reline #{Reline::VERSION} on Ruby #{RUBY_VERSION}"
puts "Submit with a blank line. Ctrl-D or 'exit' to leave."
$stdout.flush

# One binding shared by every evaluation, so that local variables assigned at the
# prompt are still there on the next line.
context = binding

begin
  loop do
    # readmultiline keeps collecting lines until the block reports the buffer
    # complete. A blank line terminates it, which avoids needing a parser to
    # decide whether an expression is finished.
    buffer = Reline.readmultiline('ruby> ', true) do |input|
      !input.strip.empty? && input.end_with?("\n\n")
    end

    break if buffer.nil? # Ctrl-D

    source = buffer.strip
    next if source.empty?
    break if source == 'exit'

    begin
      puts context.eval(source).inspect
    rescue Exception => e
      puts "#{e.class}: #{e.message}"
    end
    $stdout.flush
  end
rescue Exception => e
  # Reline drives the terminal directly through io/console, which needs termios
  # and a poll-with-timeout that this component does not provide yet. Report the
  # failure rather than letting it surface as a fault.
  puts "reline unavailable: #{e.class}: #{e.message}"
  $stdout.flush
end

puts "leaving Ruby shell"
$stdout.flush
