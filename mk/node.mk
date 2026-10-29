# Shared build settings for master, slave and sensor projects.
# Paths are relative to src/<role>, where Contiki invokes make.
CONTIKI ?= ../../../contiki-ng

# Direct builds use the same defaults as the recommended shared-adaptive profile.
# build_profile.sh passes every setting explicitly for reproducible comparisons.
USE_DBSCAN ?= 1
USE_CSV_TRACES ?= 1
SENSOR_ID_OFFSET ?= 5
PIPE_SAMPLE_PHASE_FIX ?= 1
PIPE_COMPACT_LOG ?= 1
PIPE_SHARED_LENGTH ?= 3
PIPE_LEAF_NO_EB ?= 1
PIPE_EXTENDED_TIMING ?= 1
PIPE_DIAGNOSTICS ?= 0
PIPE_DYNAMIC_SCHEDULE ?= 0
PIPE_ADAPTIVE_EB ?= 1
PIPE_SCHEDULE_LENGTH ?= 17

CFLAGS += -I../../include
CFLAGS += -DPROJECT_CONF_H=\"project-conf.h\"
CFLAGS += -DUSE_DBSCAN=$(USE_DBSCAN)
CFLAGS += -DUSE_CSV_TRACES=$(USE_CSV_TRACES)
CFLAGS += -DSENSOR_ID_OFFSET=$(SENSOR_ID_OFFSET)
CFLAGS += -DPIPE_SAMPLE_PHASE_FIX=$(PIPE_SAMPLE_PHASE_FIX)
CFLAGS += -DPIPE_COMPACT_LOG=$(PIPE_COMPACT_LOG)
CFLAGS += -DPIPE_SHARED_LENGTH=$(PIPE_SHARED_LENGTH)
CFLAGS += -DPIPE_LEAF_NO_EB=$(PIPE_LEAF_NO_EB)
CFLAGS += -DPIPE_EXTENDED_TIMING=$(PIPE_EXTENDED_TIMING)
CFLAGS += -DPIPE_DIAGNOSTICS=$(PIPE_DIAGNOSTICS)
CFLAGS += -DPIPE_DYNAMIC_SCHEDULE=$(PIPE_DYNAMIC_SCHEDULE)
CFLAGS += -DPIPE_ADAPTIVE_EB=$(PIPE_ADAPTIVE_EB)
CFLAGS += -DPIPE_SCHEDULE_LENGTH=$(PIPE_SCHEDULE_LENGTH)

PROJECTDIRS += ../common
PROJECT_SOURCEFILES += mac_hash.c pipeline_schedule.c pipeline_phase.c

# Forwarding belongs to the application. Do not enable RPL for these profiles.
MAKE_MAC = MAKE_MAC_TSCH
MAKE_NET = MAKE_NET_IPV6
MAKE_ROUTING = MAKE_ROUTING_NULLROUTING
include $(CONTIKI)/Makefile.include
