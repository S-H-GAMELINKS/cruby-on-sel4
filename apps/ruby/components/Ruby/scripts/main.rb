# frozen_string_literal: true
#
# What the Steam Deck boots into: the slide deck, with Termfront behind it.
#
# The two take turns on the one console. L2 on the Steam Deck, or the hidden
# command -- up up down down left right left right B A -- on a keyboard,
# switches from whichever is showing to the other. The drivers type a single
# byte for either (HIDDEN_COMMAND_TOGGLE_BYTE in hidden_command.h); this turns
# that byte into an exception, which unwinds whatever is running through its
# own ensure blocks, so the terminal is left the way it was found.
#
# The slides come first, so that turning the machine on in front of an audience
# shows the talk.

# The embedded CPIO archive is rooted at '/', and the file shims strip the
# leading slash before looking an entry up.
$LOAD_PATH.unshift('/')
$LOAD_PATH.unshift('/termfront/lib')

require 'io/console'

# The byte the hidden command types. Must match HIDDEN_COMMAND_TOGGLE_BYTE.
TOGGLE_BYTE = "\x1c"

# The hidden command as a terminal on the serial line sends it: arrows, then
# Escape and Enter. Keys typed there never pass through a keyboard driver, so
# the command is recognised here instead, from the bytes. The Escape cannot be
# held back at this level and reaches whatever is running, but the switch
# follows at once, so all it does is leave a menu that is about to go away.
SERIAL_COMMAND = "\e[A\e[A\e[B\e[B\e[D\e[C\e[D\e[C\e\r".b

# Raised when the hidden command arrives. An Exception rather than a
# StandardError on purpose: both programs rescue StandardError around their
# reads and would otherwise swallow it.
class Toggle < Exception; end

# Every way the two programs read standard input. A read that contains the
# toggle byte, or completes the serial form of the command, raises instead of
# returning; anything else that arrived with it is dropped, since it was typed
# at a program that is about to go away.
module ToggleOnRead
  def read_nonblock(*args, **opts)
    check_toggle(super)
  end

  def sysread(*args)
    check_toggle(super)
  end

  def readpartial(*args)
    check_toggle(super)
  end

  private

  def check_toggle(data)
    return data unless data.is_a?(String)
    raise Toggle if data.include?(TOGGLE_BYTE)

    # The slides read a byte at a time and Termfront in chunks, so the command
    # is matched against the tail of everything read rather than one read.
    tail = (@toggle_tail || ''.b) + data.b
    keep = SERIAL_COMMAND.bytesize
    @toggle_tail = tail.bytesize > keep ? tail.byteslice(-keep, keep) : tail
    if @toggle_tail.end_with?(SERIAL_COMMAND)
      @toggle_tail = ''.b
      raise Toggle
    end

    data
  end
end

STDIN.singleton_class.prepend(ToggleOnRead)

# ---------------------------------------------------------------------------
# Slides
# ---------------------------------------------------------------------------

# Loading defines the deck and present_slides without starting it.
$slides_launcher = true
require 'slides'

# ---------------------------------------------------------------------------
# Termfront
# ---------------------------------------------------------------------------

# lib/termfront.rb is a list of require_relative lines followed by a few lines
# of its own. Reading the list from it, rather than copying it here, keeps this
# working across Termfront versions. The network play files are left out: they
# need socket and openssl, neither of which is built.
File.read('/termfront/lib/termfront.rb')
    .scan(%r{^require_relative "(termfront/[^"]+)"})
    .flatten
    .reject { |feature| feature.start_with?('termfront/network/') }
    .each { |feature| require feature }

module Termfront
  # Writes in the calling thread. The original hands frames to a writer thread,
  # but there are no threads here: pthread_create is refused, so Thread.new
  # raises.
  module SynchronousWriter
    def initialize(io)
      @io = io
    end

    def syswrite(data)
      TerminalOutput.write_all(@io, data)
    end

    def close; end
  end
  AsyncWriter.prepend(SynchronousWriter)

  # The Steam Deck's controls, laid out the way a console shooter has them. The
  # controller driver (hid.c) types the D-pad as the arrow keys, A as Enter and
  # B as Escape, and the buttons only its own interface reports as letters the
  # slides ignore:
  #
  #   D-pad             move and strafe     (W/A/S/D)
  #   L1 / R1           turn                (j / l -> left / right)
  #   R2                fire                (f -> Space)
  #   A                 fire                (Enter -> Space)
  #   Y                 swap weapons        (t)
  #   B                 pick up / use       (Escape -> E)
  #
  # B stays Escape in the driver, because the hidden command is typed with it,
  # and becomes E only here. That leaves the Deck no way to leave a mission;
  # the hidden command is the way out. A keyboard types the same keys, so it
  # plays the same way, and leaves with Q.
  module DeckInput
    LETTER_KEYS = {
      'w'.ord => :w, 'a'.ord => :a, 's'.ord => :s, 'd'.ord => :d,
      ' '.ord => :space, 'q'.ord => :q, 't'.ord => :t, 'e'.ord => :e,
      "\r".ord => :space, "\n".ord => :space,
      'f'.ord => :space, 'j'.ord => :left, 'l'.ord => :right
    }.freeze

    ARROW_KEYS = {
      'A'.ord => :w, 'B'.ord => :s, 'C'.ord => :d, 'D'.ord => :a
    }.freeze

    def read_keys(stdin)
      keys = []

      while IO.select([stdin], nil, nil, 0)
        begin
          @pending << stdin.read_nonblock(64)
        rescue IO::WaitReadable
          break
        end
      end

      i = 0
      while i < @pending.bytesize
        ch = @pending.getbyte(i)
        if ch == 27 && @pending.getbyte(i + 1) == 91 && (code = @pending.getbyte(i + 2))
          key = ARROW_KEYS[code]
          keys << key if key
          i += 3
        elsif ch == 27
          keys << :e
          i += 1
        else
          key = LETTER_KEYS[ch]
          keys << key if key
          i += 1
        end
      end

      @pending.clear
      keys
    end
  end
  Input.prepend(DeckInput)

  # The title screen chooses by letter, which the Deck cannot type. This one
  # chooses with the D-pad and A instead, and drops PvP, which needs the network.
  module DeckTitleScreen
    DECK_MENU = [
      ['Wavesfight', :wavesfight],
      ['Campaign', :campaign],
      ['Training', :singleplayer]
    ].freeze

    def show
      @title_spin = 0.0
      @demo_wp_idx = 0
      @demo_wp_t = 0.0
      @demo_fire = 0
      @deck_selected ||= 0

      TerminalOutput.write_all(@stdout, TerminalOutput.begin_frame(home: true, clear: true) + TerminalOutput.end_frame)

      STDIN.raw do |stdin|
        last_render = clock - Config::RENDER_DT
        loop do
          now = clock
          @title_spin += 0.015

          if now - last_render >= Config::RENDER_DT
            render
            last_render = now
          end

          while IO.select([stdin], nil, nil, 0)
            begin
              data = stdin.read_nonblock(64)
            rescue IO::WaitReadable
              break
            end

            i = 0
            while i < data.bytesize
              b = data.getbyte(i)
              if b == 27 && data.getbyte(i + 1) == 91
                case data.getbyte(i + 2)
                when 65 then move_selection(-1)
                when 66 then move_selection(1)
                end
                i += 3
              elsif [13, 10].include?(b)
                return DECK_MENU[@deck_selected][1]
              else
                # Escape is not a way out: there is nowhere to go back to.
                i += 1
              end
            end
          end

          spent = clock - now
          remain = Config::FRAME_DT - spent
          sleep(remain) if remain > 0
        end
      end
    end

    private

    def move_selection(step)
      @deck_selected = (@deck_selected + step) % DECK_MENU.size
      # The menu lines are cached per width; forget them so the highlight moves.
      @static_lines_cols = 0
    end

    def ensure_static_lines(cols)
      return if @static_lines_cols == cols

      super
      @static_menu_lines = DECK_MENU.each_with_index.map do |(label, _), idx|
        text = idx == (@deck_selected || 0) ? "\e[1;97;44m  #{label.ljust(12)}  \e[0m" : "\e[97m  #{label.ljust(12)}  \e[0m"
        ic = [(cols - 16) / 2 + 1, 1].max
        TerminalOutput.fit_ansi("#{' ' * (ic - 1)}#{text}", cols)
      end
    end
  end
  TitleScreen.prepend(DeckTitleScreen)

  # Co-op is the other choice here, and it needs the network.
  module SoloWavesfight
    private

    def show_wavesfight_mode_select
      :solo
    end
  end
  Game.prepend(SoloWavesfight)
end

# ---------------------------------------------------------------------------

mode = :slides

loop do
  begin
    if mode == :slides
      # Termfront shows the cursor again on its way out, which on the deck
      # would sit as a block at the end of the header.
      $stdout.syswrite("\e[?25l")
      # Closing the deck with q on a keyboard opens it again. Only the hidden
      # command leaves.
      present_slides
    else
      Termfront::Game.new.start
      # The Deck's title screen has no Quit, so the game coming back means it
      # crashed: it rescues the error itself and prints it on the way out.
      # Starting again at once would scroll that away, so wait for a key.
      $stdout.syswrite("\r\nPress any button to restart\r\n")
      STDIN.raw { STDIN.sysread(1) }
    end
  rescue Toggle
    rewind_slide_trail if mode == :slides
    mode = mode == :slides ? :game : :slides
  end
end
