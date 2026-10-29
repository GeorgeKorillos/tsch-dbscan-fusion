#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* Use neighbor discovery for link-local communication without router advertisements. */
#define UIP_CONF_ND6_SEND_NS 1
/* Learn link-layer neighbors from received IPv6 traffic. */
#define UIP_CONF_ND6_AUTOFILL_NBR_CACHE 1
#define UIP_CONF_ND6_SEND_RA 0
#define UIP_CONF_ND6_DEF_MAXDADNS 0

/* Keep buffers and tables within the Z1 memory budget. */
/* No 6LoWPAN reassembly buffer; application packets must fit without fragmentation. */
#define SICSLOWPAN_CONF_FRAG 0

/* The application uses UDP only. */
#define UIP_CONF_TCP 0

/* IPv6 buffer size; recheck packet sizes before extending any wire structure. */
#define UIP_CONF_BUFFER_SIZE 120

/* Request no IPv6 route entries. Effective route limits must be checked with viewconf
 * for this Contiki revision. */
#define UIP_CONF_MAX_ROUTES 0
#define NBR_TABLE_CONF_MAX_NEIGHBORS 8

/* Bound MAC queue buffers and reserve one UDP connection per slave direction. */
#define QUEUEBUF_CONF_NUM 8
#define UIP_CONF_UDP_CONNS 3

/* The application starts TSCH after configuring its schedule and coordinator role. */
#define TSCH_CONF_AUTOSTART 0

/* Select a TSCH time source from EBs independently of the application parent. */
#define TSCH_CONF_AUTOSELECT_TIME_SOURCE 1

#ifndef PIPE_SAMPLE_PHASE_FIX
#    define PIPE_SAMPLE_PHASE_FIX 0
#endif
#ifndef PIPE_COMPACT_LOG
#    define PIPE_COMPACT_LOG 0
#endif
#ifndef PIPE_SHARED_LENGTH
#    define PIPE_SHARED_LENGTH 3
#endif
#if PIPE_SHARED_LENGTH != 3 && PIPE_SHARED_LENGTH != 5
#    error "Supported shared periods: 3 or 5"
#endif
#ifndef PIPE_LEAF_NO_EB
#    define PIPE_LEAF_NO_EB 0
#endif
#define PIPE_EB_ENABLED 1
#ifndef PIPE_EXTENDED_TIMING
#    define PIPE_EXTENDED_TIMING 0
#endif
#ifndef PIPE_DIAGNOSTICS
#    define PIPE_DIAGNOSTICS 0
#endif

/* Build either the common shared schedule or autonomous receiver cells. */
#ifndef PIPE_DYNAMIC_SCHEDULE
#    define PIPE_DYNAMIC_SCHEDULE 1
#endif
#ifndef PIPE_ADAPTIVE_EB
#    define PIPE_ADAPTIVE_EB 1
#endif
#ifndef PIPE_SCHEDULE_LENGTH
#    define PIPE_SCHEDULE_LENGTH 17
#endif
#if PIPE_DYNAMIC_SCHEDULE
#    if PIPE_SCHEDULE_LENGTH != 11 && PIPE_SCHEDULE_LENGTH != 17 && PIPE_SCHEDULE_LENGTH != 31
#        error "Supported dynamic periods: 11, 17 or 31 (same on every node)"
#    endif
#    define TSCH_SCHEDULE_CONF_WITH_6TISCH_MINIMAL 0
#    define TSCH_CONF_INIT_SCHEDULE_FROM_EB 0
#    define TSCH_SCHEDULE_CONF_MAX_SLOTFRAMES 2
#    define TSCH_SCHEDULE_CONF_MAX_LINKS 12
#    define TSCH_CONF_WITH_LINK_SELECTOR 1
#    define TSCH_CALLBACK_PACKET_READY pipeline_schedule_packet_ready
#else
#    define TSCH_SCHEDULE_CONF_WITH_6TISCH_MINIMAL 1
#    define TSCH_SCHEDULE_CONF_DEFAULT_LENGTH PIPE_SHARED_LENGTH
#    define TSCH_CONF_INIT_SCHEDULE_FROM_EB 1
#    define TSCH_SCHEDULE_CONF_MAX_SLOTFRAMES 1
#    define TSCH_SCHEDULE_CONF_MAX_LINKS 1
#    define TSCH_CONF_WITH_LINK_SELECTOR 0
#endif
#if PIPE_EB_ENABLED
#    define TSCH_CONF_EB_PERIOD (4 * CLOCK_SECOND)
#    if PIPE_ADAPTIVE_EB
#        define TSCH_CONF_MAX_EB_PERIOD (16 * CLOCK_SECOND)
#        define TSCH_CALLBACK_NEW_TIME_SOURCE pipeline_schedule_new_time_source
#        define TSCH_CALLBACK_JOINING_NETWORK pipeline_schedule_reset_eb
#        define TSCH_CALLBACK_LEAVING_NETWORK pipeline_schedule_reset_eb
#    else
#        define TSCH_CONF_MAX_EB_PERIOD (4 * CLOCK_SECOND)
#    endif
#else
/* Leaf-only profiles disable EB transmission, not reception or TSCH synchronization.
 * Infrastructure nodes must provide synchronization coverage. */
#    define TSCH_CONF_EB_PERIOD 0
#    define TSCH_CONF_MAX_EB_PERIOD 0
#endif
#define TSCH_PACKET_CONF_EB_WITH_SLOTFRAME_AND_LINK 0

/* All roles must use the same hopping sequence. */
#define TSCH_CONF_DEFAULT_HOPPING_SEQUENCE TSCH_HOPPING_SEQUENCE_4_4
#define TSCH_CONF_JOIN_HOPPING_SEQUENCE TSCH_HOPPING_SEQUENCE_4_4

/* Per-slot logging is disabled to keep interrupt logging overhead small. */
#define TSCH_LOG_CONF_PER_SLOT 0

/* Retain network warnings and errors during normal measurements. */
#define LOG_CONF_LEVEL_TCPIP LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_IPV6 LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_MAC LOG_LEVEL_WARN

#endif
