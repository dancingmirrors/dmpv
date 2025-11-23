#ifndef DMPV_DMPV_H
#define DMPV_DMPV_H

// This should be accessed by glue code only, never normal code.
// The only purpose of this is to make dmpv library-safe.
// Think hard before adding new members.
struct dmpv_global {
    struct mp_log *log;
    struct m_config_shadow *config;
    struct mp_client_api *client_api;
    char *configdir;
    struct stats_base *stats;
    struct demux_packet_pool *packet_pool;
};

#endif
