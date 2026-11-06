##
## Makefile
##
## Copyright (C) 2026, Charles Chiou
##

SHELL := /bin/bash
BUILD_DIR := build
NUM_PROCS := $(shell nproc 2>/dev/null || echo 4)

.PHONY: all clean distclean submodules test

all: submodules
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && (test -f Makefile || cmake .. -DCMAKE_BUILD_TYPE=Release)
	@$(MAKE) -C $(BUILD_DIR) -j$(NUM_PROCS)

test: all
	@./build/test_xymodem
	@./build/test_terminal_sanitizer
	@./build/test_mcu_polling
	@if [ -f ./build/test_serial_isolation ]; then ./build/test_serial_isolation; fi
	@./build/test_mcp_streamable_http

submodules:
	@if [ -f .gitmodules ]; then \
		git submodule update --init --recursive; \
	fi

clean:
	@if [ -d $(BUILD_DIR) ] && [ -f $(BUILD_DIR)/Makefile ]; then \
		$(MAKE) -C $(BUILD_DIR) clean; \
	fi

distclean:
	@rm -rf $(BUILD_DIR)
