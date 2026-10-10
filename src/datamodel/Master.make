# Master.make for datamodel subdirectory

# ============================================================================
# Configuration
# ============================================================================

# Use variables passed from parent Makefile, or set defaults
CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O2 -g -pthread
# Every source uses project-root-relative includes (see placer/Master.make).
override INCLUDES := -I..
OBJ_DIR ?= ../build/obj/datamodel

# Source files in this directory
SRCS := kt_die.cc kt_dm.cc kt_graph.cc kt_solutionMgr.cc

# Only include files that exist
EXISTING_SRCS := $(foreach src,$(SRCS),$(if $(wildcard $(src)),$(src),))

# Object files
OBJS := $(EXISTING_SRCS:%.cc=$(OBJ_DIR)/%.o)

# Dependency files
DEPS := $(OBJS:.o=.d)

# ============================================================================
# Targets
# ============================================================================

.PHONY: all clean objlist

# Default: build objects
all: $(OBJS)

# Compile source files
$(OBJ_DIR)/%.o: %.cc
	@mkdir -p $(OBJ_DIR)
	@echo "  Compiling $<..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# Clean this directory
clean:
	@rm -f $(OBJS) $(DEPS)

# Return object list (used by parent Makefile)
objlist:
	@echo "$(OBJS)"

