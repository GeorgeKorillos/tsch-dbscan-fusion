# Build the three node roles and inspect the firmware already on disk.
# Named experiment profiles are selected with tools/build_profile.sh.
TARGET ?= z1
SIZE ?= size
PROF_FILE ?= output_prof.txt
ROLES = master slave sensor

# These defaults match shared-adaptive with a three-slot shared frame.
# 1 to use DBSCAN algorithm, 0 to use simple Weighted Average
export USE_DBSCAN ?= 1

# 1 to use CSV traces (1D data), 0 to use Random FFTs (8D data)
export USE_CSV_TRACES ?= 1

# Offset for Sensor IDs to map correctly to the CSV columns (0 to 9)
# Example: If your first Sensor in Cooja has ID 5, set OFFSET to 5.
export SENSOR_ID_OFFSET ?= 5

# Rebuild must complete clean before any compilation, even with make -j.
.NOTPARALLEL:
.PHONY: default all clean rebuild profile ramprof flashprof

# Default target
# Running just:
#   make
# will execute:
#   clean -> all -> profiling and saving to file
default: rebuild profile

# Build all projects
all:
	@set -e; for role in $(ROLES); do \
		$(MAKE) -C src/$$role TARGET=$(TARGET) all; \
	done

# Clean all projects
clean:
	@set -e; for role in $(ROLES); do \
		$(MAKE) -C src/$$role TARGET=$(TARGET) clean; \
	done
	@rm -f $(PROF_FILE)

# Full rebuild
# Executes:
#   clean -> all
rebuild:
	@$(MAKE) clean
	@$(MAKE) all

# This target reads existing binaries. It does not select or rebuild a profile.
profile:
	@printf 'Contiki-NG memory report\nTarget: %s\n' '$(TARGET)' > $(PROF_FILE)
	@set -e; for role in $(ROLES); do \
		printf '\n%s\n' "$$role" >> $(PROF_FILE); \
		$(SIZE) src/$$role/build/$(TARGET)/$${role}_node.$(TARGET) >> $(PROF_FILE); \
	done
	@$(MAKE) ramprof >> $(PROF_FILE) 2>&1
	@$(MAKE) flashprof >> $(PROF_FILE) 2>&1
	@echo 'Saved $(PROF_FILE)'

# Generate RAM Profiles for all projects
ramprof:
	@set -e; for role in $(ROLES); do \
		$(MAKE) -C src/$$role TARGET=$(TARGET) $${role}_node.ramprof; \
	done

# Generate Flash Profiles for all projects
flashprof:
	@set -e; for role in $(ROLES); do \
		$(MAKE) -C src/$$role TARGET=$(TARGET) $${role}_node.flashprof; \
	done

# =========================================================
# Usage Examples
# =========================================================
#
# Build everything using default TARGET=z1:
#   make
#
# Clean everything:
#   make clean
#
# Full rebuild:
#   make rebuild
#
# Build using another platform:
#   make TARGET=sky
#
# Full rebuild using another platform:
#   make rebuild TARGET=cooja
#
# Resulting build folders remain LOCAL to each project:
#   src/master/build/z1/
#   src/sensor/build/z1/
#   src/slave/build/z1/
#
# =========================================================