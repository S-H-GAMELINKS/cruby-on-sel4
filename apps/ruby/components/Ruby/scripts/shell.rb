=begin
require "reline"

loop do
  buffer = Reline.readmultiline("> ", true) do |input|
    !input.strip.empty? && input.end_with?("\n\n")
  end
  return if buffer.nil?
  
  eval(buffer)
end
=end

while true
  print "ruby> "
  $stdout.flush

  begin
    line = STDIN.gets
  rescue => e
    puts
    puts "stdin unavailable: #{e.class}: #{e.message}"
    $stdout.flush
    line = nil
  end

  if line.nil?
    puts "stdin closed; shell idle"
    $stdout.flush
    while true
    end
  end

  line = line.strip
  next if line.empty?
  break if line == "exit"

  puts eval(line)
  $stdout.flush
end
