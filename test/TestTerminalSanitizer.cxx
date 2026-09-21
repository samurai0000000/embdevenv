/*
 * TestTerminalSanitizer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TerminalSanitizer.hxx"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>

using namespace embdevenv;

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            g_testsFailed++; \
        } else { \
            g_testsPassed++; \
        } \
    } while (0)

static void testCprDrop()
{
    std::cout << "[TEST] testCprDrop..." << std::endl;

    // Test direct CPR strings
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[26R") == "", "Single line CPR \x1b[26R dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[41R") == "", "Single line CPR \x1b[41R dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[26;1R") == "", "Standard CPR \x1b[26;1R dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[?26;1R") == "", "DEC private CPR \x1b[?26;1R dropped");

    // Test CPR prefixed to commands (the exact symptom reported)
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[41Rprintenv\n") == "printenv\n",
                "CPR prefix stripped from printenv");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[26Rry 'help'\n") == "ry 'help'\n",
                "CPR prefix stripped from ry 'help'");
    TEST_ASSERT(TerminalSanitizer::sanitize("cmd1\n\x1b[41Rcmd2\n") == "cmd1\ncmd2\n",
                "Embedded CPR stripped between commands");
}

static void testDeviceAttributesDrop()
{
    std::cout << "[TEST] testDeviceAttributesDrop..." << std::endl;

    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[?1;2c") == "", "Primary DA dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[>0;10;1c") == "", "Secondary DA dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[c") == "", "DA \x1b[c dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[?1;2chello") == "hello", "DA stripped before text");
}

static void testDeviceStatusAndWindowReports()
{
    std::cout << "[TEST] testDeviceStatusAndWindowReports..." << std::endl;

    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[0n") == "", "DSR 0n dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[3n") == "", "DSR 3n dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[?0n") == "", "DSR ?0n dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[8;24;80t") == "", "Window size report dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b]0;title\x07") == "", "OSC BEL dropped");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b]0;title\x1b\\") == "", "OSC ST dropped");
}

static void testValidKeystrokesPreserved()
{
    std::cout << "[TEST] testValidKeystrokesPreserved..." << std::endl;

    // Arrow keys
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[A") == "\x1b[A", "ArrowUp preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[B") == "\x1b[B", "ArrowDown preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[C") == "\x1b[C", "ArrowRight preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[D") == "\x1b[D", "ArrowLeft preserved");

    // Editing & cursor keys
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[H") == "\x1b[H", "Home preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[F") == "\x1b[F", "End preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[3~") == "\x1b[3~", "Delete preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[2~") == "\x1b[2~", "Insert preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[5~") == "\x1b[5~", "PageUp preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[6~") == "\x1b[6~", "PageDown preserved");

    // Modified keys
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[1;5A") == "\x1b[1;5A", "Ctrl-Up preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[1;5C") == "\x1b[1;5C", "Ctrl-Right preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1b[Z") == "\x1b[Z", "Shift-Tab preserved");

    // SS3 function keys
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1bOP") == "\x1bOP", "F1 preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x1bOQ") == "\x1bOQ", "F2 preserved");

    // Control characters
    TEST_ASSERT(TerminalSanitizer::sanitize("\x03") == "\x03", "Ctrl-C preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x04") == "\x04", "Ctrl-D preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\r\n") == "\r\n", "CRLF preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\t") == "\t", "Tab preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("\x08") == "\x08", "Backspace preserved");

    // Plain text & UTF-8
    TEST_ASSERT(TerminalSanitizer::sanitize("help\n") == "help\n", "Standard command preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("setenv bootargs console=ttyS0\n") == "setenv bootargs console=ttyS0\n",
                "Complex command preserved");
    TEST_ASSERT(TerminalSanitizer::sanitize("テスト\n") == "テスト\n", "UTF-8 preserved");
}

static void testStreamingChunkBoundaries()
{
    std::cout << "[TEST] testStreamingChunkBoundaries..." << std::endl;

    {
        // Split CPR across 2 chunks: "\x1b" + "[26R"
        TerminalSanitizer sanitizer;
        std::string out1 = sanitizer.process("\x1b");
        std::string out2 = sanitizer.process("[26R");
        TEST_ASSERT(out1 == "" && out2 == "", "CPR split at ESC boundary dropped");
    }

    {
        // Split CPR across 2 chunks: "\x1b[" + "41Rprintenv\n"
        TerminalSanitizer sanitizer;
        std::string out1 = sanitizer.process("\x1b[");
        std::string out2 = sanitizer.process("41Rprintenv\n");
        TEST_ASSERT(out1 == "" && out2 == "printenv\n", "CPR split at CSI boundary stripped");
    }

    {
        // Split CPR across 3 chunks: "\x1b" + "[2" + "6;1R"
        TerminalSanitizer sanitizer;
        std::string out1 = sanitizer.process("\x1b");
        std::string out2 = sanitizer.process("[2");
        std::string out3 = sanitizer.process("6;1R");
        TEST_ASSERT(out1 == "" && out2 == "" && out3 == "", "CPR split across 3 chunks dropped");
    }

    {
        // Split ArrowUp across 2 chunks: "\x1b" + "[A"
        TerminalSanitizer sanitizer;
        std::string out1 = sanitizer.process("\x1b");
        std::string out2 = sanitizer.process("[A");
        TEST_ASSERT(out1 == "" && out2 == "\x1b[A", "ArrowUp split across chunks preserved");
    }

    {
        // Command stream with interspersed fragmented CPR:
        // "echo " -> "\x1b[41" -> "Rhello\n"
        TerminalSanitizer sanitizer;
        std::string out1 = sanitizer.process("echo ");
        std::string out2 = sanitizer.process("\x1b[41");
        std::string out3 = sanitizer.process("Rhello\n");
        TEST_ASSERT(out1 == "echo " && out2 == "" && out3 == "hello\n",
                    "Interspersed fragmented CPR cleanly removed");
    }

    {
        // Lone ESC flushed at stream end
        TerminalSanitizer sanitizer;
        std::string out1 = sanitizer.process("\x1b");
        std::string out2 = sanitizer.flush();
        TEST_ASSERT(out1 == "" && out2 == "\x1b", "Flush outputs pending lone ESC");
    }
}

static void testNullByteFiltering()
{
    std::cout << "[TEST] testNullByteFiltering..." << std::endl;

    std::string withNulls;
    withNulls += 'h';
    withNulls += '\0';
    withNulls += 'e';
    withNulls += '\0';
    withNulls += 'l';
    withNulls += 'p';

    TEST_ASSERT(TerminalSanitizer::sanitize(withNulls) == "help", "Null bytes filtered");
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << " Running TerminalSanitizer Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    testCprDrop();
    testDeviceAttributesDrop();
    testDeviceStatusAndWindowReports();
    testValidKeystrokesPreserved();
    testStreamingChunkBoundaries();
    testNullByteFiltering();

    std::cout << "----------------------------------------" << std::endl;
    std::cout << " Tests Passed: " << g_testsPassed << std::endl;
    std::cout << " Tests Failed: " << g_testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return g_testsFailed == 0 ? 0 : 1;
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
