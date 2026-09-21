/*
 * TerminalSanitizer.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_TERMINAL_SANITIZER_HXX
#define EMBDEVENV_TERMINAL_SANITIZER_HXX

#include <string>

namespace embdevenv {

/**
 * TerminalSanitizer filters unwanted automated terminal emulator responses
 * (ANSI Cursor Position Reports, Device Attributes, Device Status Reports,
 * OSC responses, and null bytes) from target serial input streams while
 * preserving legitimate user keystrokes (arrows, editing keys, control chars).
 */
class TerminalSanitizer {
public:
    TerminalSanitizer() = default;
    ~TerminalSanitizer() = default;

    /**
     * Stateful streaming sanitization.
     * Incomplete escape sequences at the end of @p chunk are held in @p pendingBuffer
     * and processed when subsequent chunks arrive.
     */
    static std::string sanitize(const std::string &chunk, std::string &pendingBuffer);

    /**
     * Stateless string sanitization.
     * Incomplete trailing escape sequences (if any) are evaluated immediately.
     */
    static std::string sanitize(const std::string &input);

    /**
     * Instance method for streaming sanitization maintaining internal state.
     */
    std::string process(const std::string &chunk);
    std::string process(const char *data, size_t len);

    /**
     * Flushes any pending incomplete escape buffer as raw output.
     */
    std::string flush();

    /**
     * Clears internal state buffer.
     */
    void reset();

private:
    std::string _pending;
};

} // namespace embdevenv

#endif // EMBDEVENV_TERMINAL_SANITIZER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
