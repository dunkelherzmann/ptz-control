/*
 * PTZ Control shared core: one C ABI for every native host (AppKit, Win32, Qt).
 *
 * All calls are blocking. A host must serialize calls on one session (a single
 * worker thread/queue) and never call them from its UI thread. Callbacks may run
 * on any core thread; a host may block inside prompt/confirm while it shows UI.
 */
#ifndef PTZCORE_H
#define PTZCORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTZ_TEXT 512

typedef enum { PTZ_ROUTE_LOCAL = 0, PTZ_ROUTE_SSH = 1 } ptz_route;
typedef enum { PTZ_VENDOR_ONVIF = 0, PTZ_VENDOR_AXIS = 1 } ptz_vendor;

typedef struct {
    ptz_route route;
    ptz_vendor vendor;
    const char *camera_host;
    int camera_port;          /* camera HTTP port, usually 80 */
    const char *camera_user;
    const char *camera_password;
    int https_port;           /* web-interface HTTPS fallback, usually 443 */
    int profile;              /* ONVIF media profile index */
    double timeout;           /* seconds per request */
    /* SSH route only. */
    const char *ssh_host;
    const char *ssh_user;
    const char *ssh_password; /* may be empty: keys, agent or prompts */
    int ssh_port;
    const char *ssh_identity; /* optional private key file; empty = ssh defaults */
    const char *askpass_path; /* executable that calls ptz_askpass_main() */
} ptz_connect_params;

typedef struct {
    void *context;
    /* Message for the operator's event log. */
    void (*log)(void *context, const char *line);
    /* Ask the operator for a line of text. Return 1 with a NUL-terminated
     * answer in `answer`, or 0 when cancelled. */
    int (*prompt)(void *context, const char *message, int secret, char *answer, size_t answer_size);
    /* Ask the operator a yes/no question. Return 1 for yes. */
    int (*confirm)(void *context, const char *title, const char *message);
} ptz_callbacks;

/* A reading is either measured or unavailable; never a made-up zero. */
typedef struct {
    int available;
    double value;
    char text[64];            /* camera's own digits, preserved verbatim */
} ptz_reading;

typedef struct {
    ptz_reading pan, tilt, zoom;
    char move_status[64];     /* e.g. "IDLE", "MOVING"; empty when not reported */
} ptz_position;

typedef struct {
    int available;
    double min, max;
} ptz_range;

typedef struct {
    char name[128];
    char units[256];
    ptz_range pan, tilt, zoom;
    int zoom_integer;         /* Axis zoom accepts integers only */
} ptz_info;

typedef struct {
    int has_pan, has_tilt, has_zoom; /* 0 = leave that axis unchanged */
    double pan, tilt, zoom;
} ptz_target;

typedef struct ptz_session ptz_session;

/* Returns NULL and fills `error` on failure. `cancel` may be NULL; set *cancel
 * to nonzero from another thread to abort an SSH connection in progress. */
ptz_session *ptz_connect(const ptz_connect_params *params, const ptz_callbacks *callbacks,
                         volatile int *cancel, char *error, size_t error_size);
void ptz_info_get(ptz_session *session, ptz_info *info);
int ptz_get_position(ptz_session *session, ptz_position *position, char *error, size_t error_size);
int ptz_move(ptz_session *session, const ptz_target *target, char *error, size_t error_size);
int ptz_stop(ptz_session *session, char *error, size_t error_size);
/* Probes HTTP, then HTTPS, without sending credentials. Fills a URL the host
 * may open in the browser, plus an optional notice (e.g. self-signed cert). */
int ptz_web_url(ptz_session *session, char *url, size_t url_size,
                char *notice, size_t notice_size, char *error, size_t error_size);
/* 0 once an SSH tunnel has gone away; always 1 for local sessions. */
int ptz_link_alive(ptz_session *session);
void ptz_close(ptz_session *session);

/* Hosts call this from main() when PTZ_ASKPASS_SOCKET is set, then exit with
 * its result. It relays an ssh prompt to the session that spawned ssh. */
int ptz_askpass_main(int argc, char **argv);
int ptz_is_askpass_invocation(void);

/* Parse a decimal number independent of the user's locale. Returns 1 on success. */
int ptz_parse_number(const char *text, double *value);
/* The app's version, e.g. "1.0.12": VERSION's major.minor plus commits since it changed. */
const char *ptz_core_version(void);

#ifdef __cplusplus
}
#endif

#endif
