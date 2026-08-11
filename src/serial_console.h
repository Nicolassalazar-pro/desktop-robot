// serial_console.h
// Table-driven dispatcher for the ~55-command serial console.
//
// The command handlers live in src/main.cpp because they reach ~100 globals
// (motor state, eye engine, WiFi, calibration, IMU, RF survey ...) that would
// otherwise need to be exposed via a wide interface header.  Only the
// mechanism -- table iteration, argument split, help walker -- lives here so
// the dispatch shape can be reasoned about (and eventually tested) in
// isolation, and so `help` is generated from the same table that dispatches.
//
// See docs/key-issues/READ.md: READ-5 (extract dispatchCommand), READ-2
// (help drifting out of sync with reality), READ-8 (strcmp/strncmp).
#pragma once

#include <Arduino.h>
#include <stddef.h>

// One row per top-level command.
struct SerialCommand {
  // Command word, matched EXACTLY against the pre-split first token via
  // strcmp.  Original main.cpp mixed strcmp() and
  //   strncmp(line, "X", N) == 0 && (line[N] == '\0' || line[N] == ' ')
  // but splitArgument null-terminates the command word at the first
  // whitespace before the compare runs, so `line[N] == ' '` is unreachable
  // and the two forms are behaviour-identical (READ-8).  The table uses
  // strcmp uniformly.
  const char* name;

  // Command handler.  Argument is the pre-split remainder (may be nullptr
  // or the empty string).
  void (*handler)(char* argument);

  // Static help text for this command.  May contain embedded '\n' for
  // multi-line usage.  nullptr = don't print any static help for this row
  // (e.g. a command whose help is combined with another row).
  const char* usage;

  // Optional dynamic help printer.  Used when usage cannot be a static
  // literal (e.g. `gesture` enumerates the movement pattern table at
  // runtime).  Called only when `usage` is nullptr.
  void (*helpPrinter)();

  // Optional section header printed BEFORE this row's usage, on its own
  // line.  Used to reproduce the "-- expression --", "-- network / UI --"
  // etc. dividers of the original printHelp output.
  const char* banner;
};

// Splits `line` at the first whitespace character in place: the byte at
// that position becomes '\0', so `line` is now the command word.  The
// returned pointer is the trimmed argument tail (never contains leading or
// trailing whitespace).  Returns nullptr if the line has no argument.
char* serialConsoleSplitArgument(char* line);

// Splits `line`, then scans `table[0..count)` for a name matching the
// command word (strcmp).  On match, invokes the handler with the argument.
// On no match, prints "ERR unknown command".
void serialConsoleDispatch(char* line, const SerialCommand* table,
                           size_t count);

// Walks `table[0..count)` and prints each row's banner (if any) and usage
// (via helpPrinter if usage is nullptr).  The iteration order is the
// table order.
void serialConsolePrintHelp(const SerialCommand* table, size_t count);
