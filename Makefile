CC = gcc
CPPFLAGS = -Iinclude
CFLAGS = -std=c11 -O3 -Wall -Wextra -Wpedantic -Werror
LDLIBS = -lgdi32 -luser32 -lm
SOURCES = src/main.c src/parallel.c src/raytrace.c
HEADERS = include/parallel.h include/raytrace.h
TARGET = build/raytrace.exe

.PHONY: all run test benchmark clean
all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

build:
	powershell.exe -NoProfile -Command "New-Item -ItemType Directory -Force -Path 'build' | Out-Null"

run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	./$(TARGET) --self-test

benchmark: $(TARGET)
	./$(TARGET) --benchmark

# Remove only the executable; preserve exported images and benchmark results.
clean:
	powershell.exe -NoProfile -Command "if (Test-Path -LiteralPath 'build/raytrace.exe') { Remove-Item -LiteralPath 'build/raytrace.exe' -ErrorAction Stop }"
