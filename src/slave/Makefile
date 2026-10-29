# Build the slave firmware; shared flags live in mk/node.mk.
CONTIKI_PROJECT = slave_node
all: $(CONTIKI_PROJECT)

PROJECT_SOURCEFILES += fft_processing.c clustering.c

include ../../mk/node.mk
