/*
 * TerminalSanitizer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TerminalSanitizer.hxx"
#include <cctype>
#include <algorithm>

namespace embdevenv {

std::string TerminalSanitizer::sanitize(const std::string &chunk, std::string &pendingBuffer)
{
    if (chunk.empty() && pendingBuffer.empty()) {
        return "";
    }

    std::string data;
    if (!pendingBuffer.empty()) {
        data = pendingBuffer + chunk;
        pendingBuffer.clear();
    } else {
        data = chunk;
    }

    std::string clean;
    clean.reserve(data.size());

    size_t i = 0;
    const size_t len = data.size();

    while (i < len) {
        unsigned char c = static_cast<unsigned char>(data[i]);

        // Drop null bytes (Telnet CR-NUL or binary zeros)
        if (c == '\0') {
            ++i;
            continue;
        }

        if (c == 0x1b) { // ESC
            if (i + 1 >= len) {
                // Incomplete escape sequence at the end of the buffer
                pendingBuffer = data.substr(i);
                break;
            }

            char next = data[i + 1];

            if (next == '[') { // CSI sequence: \x1b[ ...
                size_t j = i + 2;

                // Handle optional DEC private / parameter prefixes: '?', '>', '=', '<'
                if (j < len && (data[j] == '?' || data[j] == '>' || data[j] == '=' || data[j] == '<')) {
                    ++j;
                }

                // Scan parameters (digits and semicolons)
                size_t startDigits = j;
                while (j < len && (std::isdigit(static_cast<unsigned char>(data[j])) || data[j] == ';')) {
                    ++j;
                }

                if (j >= len) {
                    // Incomplete CSI sequence spanning chunk boundary
                    if (len - i < 64) {
                        pendingBuffer = data.substr(i);
                        break;
                    }
                    // Too long; flush ESC and continue
                    clean += static_cast<char>(c);
                    ++i;
                    continue;
                }

                char finalChar = data[j];

                // Check for automated terminal emulator responses to drop
                bool isAutomatedResponse = false;

                if (finalChar == 'R') {
                    // Cursor Position Report (CPR): \x1b[<row>;<col>R or \x1b[<row>R or \x1b[?<row>;<col>R
                    // Requires at least one parameter digit or semicolon
                    if (j > startDigits) {
                        isAutomatedResponse = true;
                    }
                } else if (finalChar == 'c') {
                    // Device Attributes (DA): \x1b[?<attrs>c, \x1b[<attrs>c, \x1b[><attrs>c
                    isAutomatedResponse = true;
                } else if (finalChar == 'n') {
                    // Device Status Report (DSR): \x1b[0n, \x1b[3n, \x1b[?0n
                    if (j > startDigits) {
                        isAutomatedResponse = true;
                    }
                } else if (finalChar == 't') {
                    // Window size / state reports: \x1b[8;...t, \x1b[4;...t, \x1b[1;...t
                    if (j > startDigits) {
                        isAutomatedResponse = true;
                    }
                } else if (finalChar == 'y') {
                    // Terminal self-test report: \x1b[2;...y
                    if (j > startDigits) {
                        isAutomatedResponse = true;
                    }
                }

                if (isAutomatedResponse) {
                    // Drop automated response
                    i = j + 1;
                    continue;
                }

                // Valid user keystroke or escape sequence (e.g. \x1b[A for UpArrow, \x1b[3~ for Delete)
                clean.append(data, i, j - i + 1);
                i = j + 1;
                continue;
            }

            if (next == ']') { // OSC sequence: \x1b] ... (\x07 or \x1b\)
                size_t j = i + 2;
                bool foundTerminator = false;
                while (j < len) {
                    if (data[j] == '\x07') {
                        foundTerminator = true;
                        j += 1;
                        break;
                    }
                    if (data[j] == 0x1b && j + 1 < len && data[j + 1] == '\\') {
                        foundTerminator = true;
                        j += 2;
                        break;
                    }
                    ++j;
                }

                if (!foundTerminator) {
                    if (len - i < 256) {
                        pendingBuffer = data.substr(i);
                        break;
                    }
                    // OSC too long; discard
                    i = j;
                    continue;
                }

                // Drop OSC response
                i = j;
                continue;
            }

            if (next == 'O') { // SS3 sequence: \x1bOP..\x1bOS (F1-F4)
                if (i + 2 >= len) {
                    pendingBuffer = data.substr(i);
                    break;
                }
                clean.append(data, i, 3);
                i += 3;
                continue;
            }

            // Other 2-byte escape sequences (e.g. Alt+key)
            clean.append(data, i, 2);
            i += 2;
            continue;
        }

        // Standard character
        clean += static_cast<char>(c);
        ++i;
    }

    return clean;
}

std::string TerminalSanitizer::sanitize(const std::string &input)
{
    std::string pending;
    std::string clean = sanitize(input, pending);
    if (!pending.empty()) {
        clean += pending;
    }
    return clean;
}

std::string TerminalSanitizer::process(const std::string &chunk)
{
    return sanitize(chunk, _pending);
}

std::string TerminalSanitizer::process(const char *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return "";
    }
    return sanitize(std::string(data, len), _pending);
}

std::string TerminalSanitizer::flush()
{
    std::string out = _pending;
    _pending.clear();
    return out;
}

void TerminalSanitizer::reset()
{
    _pending.clear();
}

} // namespace embdevenv

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
