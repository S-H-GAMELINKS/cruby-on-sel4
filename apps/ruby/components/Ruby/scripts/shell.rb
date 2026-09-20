# Interactive Ruby shell over the serial console, with Reline for line editing
# and method completion.
#
# The embedded CPIO archive is rooted at '/', and the file shims strip the leading
# slash before looking an entry up, so one load path entry covers everything.
$LOAD_PATH.unshift('/')

# Reline looks for ~/.inputrc, and expanding '~' without HOME set sends Ruby to
# getpwuid, which reads /etc/passwd. There is no user database here, so name the
# root of the archive instead.
ENV['HOME'] = '/'

require 'reline'

# One binding shared by every evaluation, so that local variables assigned at the
# prompt are still there on the next line, and so completion can see them.
context = binding

# Splits "117.to_" into the receiver to evaluate and the method prefix to match.
# '.' is not one of Reline's word break characters, so the whole expression
# arrives as a single completion target.
RECEIVER_CALL = /\A(?<receiver>.*[^.])\.(?<prefix>[A-Za-z_][A-Za-z0-9_]*[?!]?)?\z/

# Candidates must be full replacements for the target: Reline keeps only those
# that start with it (line_editor.rb:816) and swaps the target for whichever is
# chosen.
Reline.completion_proc = proc do |target|
  next [] if target.empty?

  if (match = RECEIVER_CALL.match(target))
    receiver = match[:receiver]
    prefix = match[:prefix].to_s

    # Completing a call means evaluating the receiver, so anything with side
    # effects runs just by asking for suggestions. That is the same bargain IRB
    # makes; the alternative needs static type information.
    begin
      value = context.eval(receiver)
    rescue Exception
      next []
    end

    value.methods
         .map(&:to_s)
         .select { |name| name.start_with?(prefix) }
         .sort
         .map { |name| "#{receiver}.#{name}" }
  else
    names = context.local_variables.map(&:to_s) +
            context.receiver.methods.map(&:to_s) +
            Object.constants.map(&:to_s)
    names.select { |name| name.start_with?(target) }.uniq.sort
  end
end

# Show the candidates as a dialog while typing rather than only on Tab.
Reline.autocompletion = true

puts "Reline #{Reline::VERSION} on Ruby #{RUBY_VERSION}"
puts "Submit with a blank line. Ctrl-D or 'exit' to leave."
$stdout.flush

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
  puts "reline unavailable: #{e.class}: #{e.message}"
  $stdout.flush
end

puts "leaving Ruby shell"
$stdout.flush
