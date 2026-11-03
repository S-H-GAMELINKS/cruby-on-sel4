# frozen_string_literal: true
#
# A presentation tool for the framebuffer console.
#
# Ported from ruby-on-bare-metal's impl/steam-deck-slide branch. Two things
# differ, both because the console underneath is not the same one.
#
# Widths are measured in columns, not characters. That console drew one cell per
# code point, so counting characters was the same as counting columns. This one
# gives a full width character two cells, as terminals do and as Reline assumes,
# so every width here has to be a display width or Japanese text lays out wrong.
#
# Magnified text is as many rows tall as it is wide. ESC [ n z scales by n + 1 in
# both directions, so a title at mode 2 covers three rows rather than the two the
# original assumed.

begin
  require 'io/console'
rescue LoadError
  # Only used for the window size, which has a fallback.
end

STDOUT.sync = true
STDERR.sync = true

TALK_SECONDS = 15 * 60
REFRESH_INTERVAL = 0.2

# The deck lives in the CPIO archive alongside this script. An argument
# overrides it, which is how the layout is checked against a different deck
# without rebuilding.
#
# ARGV is guarded because this VM is embedded: it is set up by process_options,
# which is the command-line path ruby_init never takes, so the constant may
# simply not exist here.
SLIDES_PATH = (defined?(ARGV) && ARGV[0]) || '/slides.md'

def detect_screen_size
  if $stdout.respond_to?(:winsize)
    rows, cols = $stdout.winsize
    return [cols, rows] if rows && cols && rows.positive? && cols.positive?
  end
  [80, 25]
rescue StandardError
  [80, 25]
end

SCREEN_W, SCREEN_H = detect_screen_size

# ---------------------------------------------------------------------------
# Text measurement
# ---------------------------------------------------------------------------

# Mirrors codepoint_is_wide in fb_console.c. The two have to agree: the console
# decides where a character lands and this decides where the next one is put, so
# a disagreement shows up as text drifting out of its box.
WIDE_RANGES = [
  0x1100..0x115f, 0x2e80..0x303e, 0x3041..0x33ff, 0x3400..0x4dbf,
  0x4e00..0x9fff, 0xa000..0xa4cf, 0xac00..0xd7a3, 0xf900..0xfaff,
  0xfe30..0xfe6f, 0xff00..0xff60, 0xffe0..0xffe6
].freeze

def char_width(char)
  code = char.ord
  return 2 if code > 0xffff
  WIDE_RANGES.any? { |range| range.cover?(code) } ? 2 : 1
rescue StandardError
  1
end

def utf8_text(text)
  s = text.to_s
  s = s.dup.force_encoding('UTF-8') if s.encoding.name != 'UTF-8'
  s = s.scrub('?') unless s.valid_encoding?
  s
end

def text_chars(text)
  utf8_text(text).each_char.to_a
rescue StandardError
  text.to_s.bytes.map { |b| b < 0x80 ? b.chr : '?' }
end

def text_width(text)
  text_chars(text).sum { |ch| char_width(ch) }
end

# Take as many characters as fit in `width` columns, never splitting one in half.
def take_width(text, width)
  used = 0
  taken = []
  text_chars(text).each do |ch|
    w = char_width(ch)
    break if used + w > width
    taken << ch
    used += w
  end
  [taken.join, used]
end

def fit(text, width)
  taken, used = take_width(text, width)
  taken + (' ' * [width - used, 0].max)
end

def center(text, width = SCREEN_W)
  taken, used = take_width(text, width)
  (' ' * ((width - used) / 2)) + taken
end

def wrap_text(text, width)
  width = 1 if width < 1
  lines = []
  line = +''
  used = 0

  text_chars(text).each do |ch|
    if ch == "\n"
      lines << line
      line = +''
      used = 0
      next
    end

    w = char_width(ch)
    if used + w > width
      lines << line
      line = +''
      used = 0
    end
    line << ch
    used += w
  end

  lines << line unless line.empty?
  lines.empty? ? [''] : lines
end

def clamp(value, min_value, max_value)
  return min_value if value < min_value
  return max_value if value > max_value
  value
end

def format_mmss(total_seconds)
  secs = total_seconds.to_i
  secs = 0 if secs.negative?
  format('%02d:%02d', secs / 60, secs % 60)
end

# ---------------------------------------------------------------------------
# Console
# ---------------------------------------------------------------------------

def cls
  # The magnification is reset with the screen, so a title left over from the
  # previous slide cannot make the next slide's first line tall.
  $stdout.syswrite("\x1b[2J\x1b[H\x1b[0z\x1b[0m")
end

def at(row, col)
  $stdout.syswrite("\x1b[#{row};#{col}H")
end

def color(code)
  $stdout.syswrite("\x1b[#{code}m")
end

# ESC [ n z, the console's magnification. Mode 0 is ordinary text.
def font(mode)
  $stdout.syswrite("\x1b[#{mode}z")
end

# A character at mode n is n + 1 times larger in each direction, so it covers
# that many columns per column of text and that many rows per line.
def font_span(mode)
  mode + 1
end

def font_rows(mode)
  mode + 1
end

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------

def layout_margin_cols
  SCREEN_W >= 120 ? 6 : 2
end

def max_content_w
  return 120 if SCREEN_W >= 180
  return 104 if SCREEN_W >= 120
  SCREEN_W - (layout_margin_cols * 2) + 1
end

def content_w
  @content_w ||= [[SCREEN_W - (layout_margin_cols * 2) + 1, 20].max, max_content_w].min
end

def content_col
  @content_col ||= [((SCREEN_W - content_w) / 2) + 1, 1].max
end

def title_col
  content_col
end

# Every kind of text has one size, whatever the slide holds. Text that does not
# fit is wrapped rather than shrunk, so a deck controls its layout with explicit
# line breaks (\n or <br>) instead of the tool choosing a size per slide.
TITLE_MODE = 2
SUBTITLE_MODE = 1
BODY_MODE = 1
CODE_MODE = 0
FOOTER_MODE = 1
TITLE_MAX_LINES = 2

# Widths below are in the magnified cells of their own mode, so they are divided
# by the span rather than compared against the screen directly.
def title_text_w
  @title_text_w ||= [content_w / font_span(TITLE_MODE), 10].max
end

def subtitle_text_w
  @subtitle_text_w ||= [content_w / font_span(SUBTITLE_MODE), 16].max
end

def body_text_w
  @body_text_w ||= [content_w / font_span(BODY_MODE), 10].max
end

def footer_text_w
  @footer_text_w ||= [content_w / font_span(FOOTER_MODE), 10].max
end

def code_box_w
  @code_box_w ||= [content_w / font_span(CODE_MODE), 12].max
end

def code_inner_w
  code_box_w - 4
end

def title_row
  clamp(SCREEN_H / 8, 3, [SCREEN_H - 14, 3].max)
end

def title_block_rows
  TITLE_MAX_LINES * font_rows(TITLE_MODE)
end

def subtitle_row
  title_row + title_block_rows
end

def body_start_row(has_subtitle)
  has_subtitle ? subtitle_row + font_rows(SUBTITLE_MODE) + 1 : title_row + title_block_rows + 1
end

def body_last_row
  SCREEN_H - 5
end

def title_slide_title_row
  clamp((SCREEN_H - title_block_rows) / 2, 3, [SCREEN_H - title_block_rows - 2, 3].max)
end

Slide = Struct.new(:title, :subtitle, :bullets, :code, :image, :notes, :layout, keyword_init: true)

# A bullet and how deeply it is nested: 0 for a top level item, 1 for one
# indented under it, and so on.
Bullet = Struct.new(:text, :level)

# ---------------------------------------------------------------------------
# Input
# ---------------------------------------------------------------------------

def read_key
  c = $stdin.sysread(1)
  return :enter if ["\n", "\r"].include?(c)
  return :space if c == ' '
  return :back if ["\x7f", "\b"].include?(c)
  return :quit if ['q', 'Q'].include?(c)
  return :home if ['g', 'G'].include?(c)
  return :end if ['e', 'E'].include?(c)

  return c unless c == "\x1b"

  c2 = begin
    $stdin.sysread(1)
  rescue StandardError
    nil
  end
  return :escape unless c2 == '['

  c3 = begin
    $stdin.sysread(1)
  rescue StandardError
    nil
  end
  case c3
  when 'D' then :left
  when 'C' then :right
  when 'A' then :up
  when 'B' then :down
  when '5', '6'
    tail = begin
      $stdin.sysread(1)
    rescue StandardError
      nil
    end
    return :escape unless tail == '~'
    c3 == '5' ? :page_up : :page_down
  else :escape
  end
end

# Wait for a key, but not forever: the clock in the header has to keep moving.
# IO.select is what makes that one wait rather than a poll loop.
def poll_key(timeout)
  return nil unless IO.select([$stdin], nil, nil, timeout)
  read_key
rescue StandardError
  nil
end

# ---------------------------------------------------------------------------
# Slides
# ---------------------------------------------------------------------------

def code_excerpt(path, start_line: 1, lines: 8)
  src = File.read(path).split("\n")
  (src[start_line - 1, lines] || []).map.with_index do |line, idx|
    "#{(start_line + idx).to_s.rjust(3)}: #{line}"
  end
rescue StandardError
  ["code not embedded: #{path}"]
end

def parse_code_directive(line)
  m = line.match(/\A\{\{code:(.+?):(\d+):(\d+)\}\}\z/)
  return nil unless m
  code_excerpt(m[1], start_line: m[2].to_i, lines: m[3].to_i)
end

# The width and height of a binary PPM, from its header alone. Comments are
# dropped before matching, which may also eat into the samples that follow the
# header, but only the header is looked at.
def ppm_size(path)
  header = File.open(path, 'rb') { |f| f.read(256) }.to_s.b
  m = header.gsub(/#[^\n]*/n, '').match(/\AP6\s+(\d+)\s+(\d+)/n)
  m && [m[1].to_i, m[2].to_i]
rescue StandardError
  nil
end

def parse_image_directive(line)
  m = line.match(/\A\{\{image:(.+?)\}\}\z/)
  m && m[1]
end

# Two spaces of indentation per level, as Markdown writes nested lists. Deeper
# than two levels is drawn at two: there is no width left for more.
def bullet_level(line)
  [line[/\A */].size / 2, 2].min
end

def expand_line_breaks(text)
  text.to_s.gsub(%r{<br\s*/?>}i, "\n").gsub('\\n', "\n")
end

def parse_markdown(text)
  text.split(/^---+\s*$/).filter_map do |chunk|
    slide = Slide.new(title: nil, subtitle: nil, bullets: [], code: [], image: nil, notes: nil,
                      layout: 'default')
    in_code = false
    code_lines = []

    chunk.lines.map(&:chomp).each do |line|
      stripped = line.strip

      if in_code
        if stripped.start_with?('```')
          in_code = false
          slide.code.concat(code_lines)
          code_lines = []
        else
          code_lines << line
        end
        next
      end

      next if stripped.empty?

      case stripped
      when /\A```/ then in_code = true
      when /\Alayout:/ then slide.layout = stripped.split(':', 2)[1].to_s.strip
      when /\A# / then slide.title = expand_line_breaks(stripped[2..])
      when /\A## / then slide.subtitle = stripped[3..]
      when /\A- / then slide.bullets << Bullet.new(expand_line_breaks(stripped[2..]), bullet_level(line))
      when /\A> /
        note = stripped[2..]
        slide.notes = slide.notes ? "#{slide.notes}  #{note}" : note
      else
        image = parse_image_directive(stripped)
        if image
          slide.image = image
        else
          code = parse_code_directive(stripped)
          slide.code.concat(code) if code
        end
      end
    end

    slide.code = nil if slide.code.empty?
    slide.bullets = nil if slide.bullets.empty?
    slide.title ? slide : nil
  end
end

def fallback_slides
  [Slide.new(
    title: 'CRuby on seL4',
    subtitle: "#{SLIDES_PATH} not found",
    bullets: [Bullet.new('Add it to the CPIO archive', 0)],
    notes: 'Right/Space next  Left/Back prev  q quit',
    layout: 'title'
  )]
end

def load_slides
  parsed = parse_markdown(utf8_text(File.read(SLIDES_PATH)))
  parsed.empty? ? fallback_slides : parsed
rescue StandardError
  fallback_slides
end

# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def write_at(row, col, text, mode, code)
  color(code)
  at(row, col)
  font(mode)
  $stdout.syswrite(text)
  font(0)
  color('0')
end

def render_centered(row, text, mode, code)
  span = font_span(mode)
  col = [((SCREEN_W - (text_width(text) * span)) / 2) + 1, 1].max
  write_at(row, col, text, mode, code)
end

def render_header(index, total, remaining_seconds)
  line = "[CRuby on seL4]  #{index + 1}/#{total}  " \
         "left:#{total - index - 1}  timer:#{format_mmss(remaining_seconds)}"
  write_at(1, 1, fit(line, SCREEN_W), 0, '36')
end

def render_big_title(row, text)
  wrap_text(text, title_text_w).first(TITLE_MAX_LINES).each_with_index do |line, idx|
    write_at(row + (idx * font_rows(TITLE_MODE)), title_col,
             fit(line, title_text_w), TITLE_MODE, '1;36')
  end
end

def render_title(title, subtitle)
  render_big_title(title_row, title)
  return unless subtitle
  write_at(subtitle_row, title_col, fit(subtitle, subtitle_text_w), SUBTITLE_MODE, '1;37')
end

# A top level item takes a dash; a nested one is indented and takes a dot, so
# the two read as different kinds of line rather than as one long list.
def bullet_marker(level)
  level.zero? ? '- ' : ('  ' * level) + '・'
end

def render_bullets(items, start_row)
  return start_row if items.nil? || items.empty?

  step = font_rows(BODY_MODE)
  row = start_row

  items.each_with_index do |item, i|
    # A blank row separates top level items. Nested ones stay against the item
    # they belong to.
    row += 1 if i.positive? && item.level.zero?

    marker = bullet_marker(item.level)
    # Continuation lines, whether from a line break in the deck or from
    # wrapping, are indented under the text rather than under the marker.
    indent = ' ' * text_width(marker)
    wrap_text(item.text, [body_text_w - text_width(marker), 1].max).each_with_index do |line, idx|
      break if row + step - 1 > body_last_row
      write_at(row, content_col, fit((idx.zero? ? marker : indent) + line, body_text_w), BODY_MODE, '37')
      row += step
    end
  end

  # One row of space below the list, before whatever follows it.
  row + 1
end

def render_code(lines, start_row)
  return start_row if lines.nil? || lines.empty?

  step = font_rows(CODE_MODE)
  rule = '+' + ('-' * (code_box_w - 2)) + '+'
  write_at(start_row, content_col, rule, CODE_MODE, '36')
  row = start_row + step

  # A line too long for the box is wrapped rather than cut, so nothing written in
  # the deck goes missing; breaking it by hand reads better.
  lines.flat_map { |line| wrap_text(line, code_inner_w) }.each do |line|
    break if row + step - 1 > body_last_row
    write_at(row, content_col, '| ' + fit(line, code_inner_w) + ' |', CODE_MODE, '36')
    row += step
  end

  if row + step - 1 <= SCREEN_H - 4
    write_at(row, content_col, rule, CODE_MODE, '36')
    row += step
  end

  row
end

# An image is drawn by the console itself, from the archive: this only says
# where. One image pixel is one console pixel before scaling, so a cell is 8x16
# of them, and the image is centred on that.
def render_image(path, start_row)
  return start_row unless path

  size = ppm_size(path)
  unless size
    write_at(start_row, content_col, fit("image not embedded: #{path}", content_w), 0, '31')
    return start_row + 1
  end

  cols = (size[0] + 7) / 8
  rows = (size[1] + 15) / 16
  at(start_row, [((SCREEN_W - cols) / 2) + 1, 1].max)
  $stdout.syswrite("\x1b_img;#{path}\x1b\\")
  start_row + rows
end

def render_footer(text)
  return unless text
  write_at(SCREEN_H - 1, content_col, fit(text, footer_text_w), FOOTER_MODE, '36')
end

def render_title_slide(slide, index, total, remaining)
  render_header(index, total, remaining)
  row = title_slide_title_row
  render_big_title(row, slide.title)
  if slide.subtitle
    write_at(row + title_block_rows, title_col,
             fit(slide.subtitle, subtitle_text_w), SUBTITLE_MODE, '1;37')
  end
  render_footer(slide.notes)
end

# One idea per slide, as large as it will go.
TAKAHASHI_MODE = 3

def takahashi_lines(text, mode)
  span = font_span(mode)
  width = [[(SCREEN_W - 4) / span, 1].max, 18].min
  expand_line_breaks(text).split("\n").flat_map { |line| wrap_text(line, width) }
end

def render_takahashi_slide(slide, index, total, remaining)
  render_header(index, total, remaining)

  # A row of space between lines. Shinonome draws to the full height of its
  # box, so lines set edge to edge would touch.
  glyph_h = font_rows(TAKAHASHI_MODE)
  step = glyph_h + 1
  lines = takahashi_lines(slide.title, TAKAHASHI_MODE)
  lines = lines.first([(SCREEN_H - 5 + 1) / step, 1].max)
  block_h = ((lines.length - 1) * step) + glyph_h
  row = [((SCREEN_H - block_h) / 2) + 1, 3].max

  lines.each_with_index do |line, idx|
    render_centered(row + (idx * step), line, TAKAHASHI_MODE, '1;33')
  end

  if slide.subtitle
    sub_row = row + block_h + 1
    render_centered(sub_row, slide.subtitle, 1, '1;36') if sub_row < SCREEN_H - 1
  end

  render_footer(slide.notes)
end

def render_slide(slide, index, total, remaining)
  cls

  case slide.layout
  when 'title'
    render_title_slide(slide, index, total, remaining)
  when 'takahashi', 'big'
    render_takahashi_slide(slide, index, total, remaining)
  else
    render_header(index, total, remaining)
    render_title(slide.title, slide.subtitle)
    row = render_bullets(slide.bullets, body_start_row(!slide.subtitle.nil?))
    row = render_code(slide.code, row + 1)
    render_image(slide.image, row + 1)
    render_footer(slide.notes || 'Right/Space next  Left/Back prev  q quit')
  end
end

# ---------------------------------------------------------------------------

SLIDES = load_slides

start_time = Time.now
index = 0
shown_index = nil
shown_timer = nil

loop do
  remaining = TALK_SECONDS - (Time.now - start_time)
  timer = format_mmss(remaining)

  if index != shown_index
    render_slide(SLIDES[index], index, SLIDES.length, remaining)
    shown_index = index
    shown_timer = timer
  elsif timer != shown_timer
    # Only the header changed, and repainting the whole slide to move a clock
    # would be visible as a flicker on an uncached framebuffer.
    render_header(index, SLIDES.length, remaining)
    shown_timer = timer
  end

  key = poll_key(REFRESH_INTERVAL)
  next unless key

  case key
  when :right, :down, :space, :enter, :page_down, 'n', 'N'
    index += 1 if index + 1 < SLIDES.length
  when :left, :up, :back, :page_up, 'p', 'P'
    index -= 1 if index.positive?
  when :home then index = 0
  when :end then index = SLIDES.length - 1
  when :quit, :escape then break
  end
end

cls
at(SCREEN_H / 2, 1)
$stdout.syswrite(center('slides closed'))
$stdout.syswrite("\n")
