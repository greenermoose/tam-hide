CC ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -O2 -g
LDFLAGS ?=

BUILD := build

.PHONY: all test clean

all: $(BUILD)/tam-hided $(BUILD)/tam-hidectl

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tam-hided: src/tam-hided.c src/controller.c src/vt.c src/controller.h src/vt.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/tam-hided.c src/controller.c src/vt.c $(LDFLAGS)

$(BUILD)/tam-hidectl: src/tam-hidectl.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/tam-hidectl.c $(LDFLAGS)

$(BUILD)/test_controller: tests/test_controller.c src/controller.c src/vt.c src/controller.h src/vt.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_controller.c src/controller.c src/vt.c $(LDFLAGS)

test: $(BUILD)/test_controller
	./$(BUILD)/test_controller

clean:
	rm -rf $(BUILD)
