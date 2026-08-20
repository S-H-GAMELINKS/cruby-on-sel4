#=begin
require "reline"

loop do
  buffer = Reline.readmultiline("> ", true) do |input|
    !input.strip.empty? && input.end_with?("\n\n")
  end
  return if buffer.nil?
  
  eval(buffer)
end
#=end

puts "Ruby script loaded from CPIO"
puts RUBY_VERSION
puts (1 + 1)
