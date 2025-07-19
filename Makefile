# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++23 -Wall -O2
LDFLAGS = -pthread

# Source and target files
SRCS = anifetch.cpp
TARGET = anifetch

# Default target: build the executable
.PHONY: all
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRCS)

# Installation target: builds the executable and informs the user.
# The program remains self-contained in the project directory.
.PHONY: install
install: all
	@echo "'$(TARGET)' has been built in the current directory."
	@echo "To use it, run './$(TARGET)' and your arguments."

# Clean target: remove the executable and the cache directory
.PHONY: clean
clean:
	@echo "Removing executable and cache..."
	rm -f $(TARGET)
	rm -rf .cache
	@echo "Cleanup complete."

# Uninstall target: for this project, uninstalling is the same as cleaning
.PHONY: uninstall
uninstall: clean
	@echo "'$(TARGET)' and the .cache directory have been removed."
