// serial_console.cpp
// Extracted from src/main.cpp: the ~1800-line if/return dispatchCommand
// chain becomes a table of {name, handler, usage} rows, with `help`
// generated from the same table so it cannot drift (READ-5, READ-2).
//
// This translation unit contains only the mechanism -- the individual
// command bodies stay in main.cpp because they reach the vast globals-
// backed state the robot owns.  A partial, safe extraction beats a
// complete, risky one (see READ-12).
#include "serial_console.h"

#include <ctype.h>
#include <string.h>

char* serialConsoleSplitArgument(char* line) {
  char* cursor = line;
  while ((*cursor != '\0') && !isspace(static_cast<unsigned char>(*cursor))) {
    ++cursor;
  }
  if (*cursor == '\0') {
    return nullptr;
  }

  *cursor = '\0';
  ++cursor;
  while ((*cursor != '\0') && isspace(static_cast<unsigned char>(*cursor))) {
    ++cursor;
  }

  char* end = cursor + strlen(cursor);
  while ((end > cursor) && isspace(static_cast<unsigned char>(*(end - 1)))) {
    --end;
  }
  *end = '\0';
  return cursor;
}

void serialConsoleDispatch(char* line, const SerialCommand* table,
                           size_t count) {
  char* argument = serialConsoleSplitArgument(line);
  for (size_t i = 0; i < count; ++i) {
    if (table[i].name == nullptr) {
      continue;
    }
    if (strcmp(line, table[i].name) == 0) {
      if (table[i].handler != nullptr) {
        table[i].handler(argument);
      }
      return;
    }
  }
  Serial.println(F("ERR unknown command"));
}

void serialConsolePrintHelp(const SerialCommand* table, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (table[i].banner != nullptr) {
      Serial.println(table[i].banner);
    }
    if (table[i].usage != nullptr) {
      // Serial.println emits its own '\n', so pre-existing '\n' inside
      // the usage string must be split so each segment lands on its own
      // Serial.println (matches the original printHelp() line-by-line
      // output byte-for-byte).
      const char* seg_start = table[i].usage;
      const char* p = seg_start;
      while (*p != '\0') {
        if (*p == '\n') {
          char buf[256];
          size_t seg_len = static_cast<size_t>(p - seg_start);
          if (seg_len >= sizeof(buf)) seg_len = sizeof(buf) - 1;
          memcpy(buf, seg_start, seg_len);
          buf[seg_len] = '\0';
          Serial.println(buf);
          seg_start = p + 1;
        }
        ++p;
      }
      Serial.println(seg_start);
    } else if (table[i].helpPrinter != nullptr) {
      table[i].helpPrinter();
    }
  }
}
