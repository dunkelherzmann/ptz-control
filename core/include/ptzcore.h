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
    int view_moves;           /* ptz_move_in_view() works on this camera */
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
/* A still picture of the camera's current view (JPEG or PNG bytes) and its size in
 * pixels. Free *data with ptz_free(). */
int ptz_picture(ptz_session *session, unsigned char **data, size_t *size, int *width, int *height,
                char *error, size_t error_size);
void ptz_free(void *data);
/* Moves the camera so that the point (x, y) of the last picture comes to its centre.
 * x and y run from -1 at the picture's left and bottom edges to 1 at its right and
 * top edges. Needs info.view_moves and a picture taken first. */
int ptz_move_in_view(ptz_session *session, double x, double y, char *error, size_t error_size);
/* ---- Live video -------------------------------------------------------------
 * The camera's live video, played from its RTSP stream (ONVIF GetStreamUri for the
 * chosen profile; Axis /axis-media/media.amp) over one TCP connection, through the SSH
 * tunnel when there is one. The core hands over compressed frames; the host decodes them
 * with the platform's own video decoder. */
typedef enum { PTZ_CODEC_H264 = 1, PTZ_CODEC_H265 = 2 } ptz_codec;

typedef struct {
    ptz_codec codec;
    const unsigned char *data; /* one frame in Annex B form (00 00 00 01 before each NAL unit);
                                  key frames start with the parameter sets (VPS, SPS, PPS) */
    size_t size;
    int keyframe;
    double time;               /* seconds since the first frame, from the camera's clock */
} ptz_frame;

typedef struct {
    void *context;
    /* Called on the stream's own thread for every frame, the first one a key frame. The
     * data is valid only during the call. Must not call ptz_live_stop. */
    void (*frame)(void *context, const ptz_frame *frame);
    /* Called once on the stream's thread when the stream ends by itself (never after
     * ptz_live_stop), with the reason. */
    void (*ended)(void *context, const char *reason);
} ptz_live_callbacks;

typedef struct ptz_live ptz_live;

/* Asks the camera for its video and starts it. Blocks until the camera is sending (like
 * the other session calls, on the session's worker). Returns NULL and fills `error` on
 * failure. `description` (may be NULL) gets e.g. "H.264 over RTSP". */
ptz_live *ptz_live_start(ptz_session *session, const ptz_live_callbacks *callbacks, char *description,
                         size_t description_size, char *error, size_t error_size);
/* Stops the video and waits for its thread; callbacks have ended when it returns. Call it
 * before ptz_close, from any thread except the stream's own. */
void ptz_live_stop(ptz_live *live);

/* Probes HTTP, then HTTPS, without sending credentials. Fills a URL the host
 * may open in the browser, plus an optional notice (e.g. self-signed cert). */
int ptz_web_url(ptz_session *session, char *url, size_t url_size,
                char *notice, size_t notice_size, char *error, size_t error_size);
/* 0 once an SSH tunnel has gone away; always 1 for local sessions. */
int ptz_link_alive(ptz_session *session);
void ptz_close(ptz_session *session);

/* ---- Login vault ------------------------------------------------------------
 * Saved SSH logins and saved camera logins: two independent stores in one file,
 * both unlocked by the same Administrator Password. Without one, logins are kept
 * as plain text, which needs the operator's explicit choice. Vault calls are
 * quick except unlocking and setting the Administrator Password, which derive a
 * key (about a second); hosts may call them from the UI thread or a worker, but
 * never from two threads at once. */
typedef enum { PTZ_STORE_SSH = 0, PTZ_STORE_CAMERA = 1 } ptz_store;

typedef enum {
    PTZ_VAULT_NEW = 0,      /* nothing saved yet: ask how to keep logins */
    PTZ_VAULT_PLAIN = 1,    /* kept as plain text, by the operator's choice */
    PTZ_VAULT_LOCKED = 2,   /* encrypted, needs the Administrator Password */
    PTZ_VAULT_UNLOCKED = 3, /* encrypted, open for this session */
} ptz_vault_state;

/* A saved login is a name (unique in its store, ignoring case), a username and its
 * password; hosts and ports aren't saved. */
typedef struct {
    const char *name;
    const char *user;
    const char *password;  /* may be empty */
} ptz_login;

typedef struct ptz_vault ptz_vault;

/* Opens the vault file at `path` (a missing file is a new vault). Returns NULL
 * and fills `error` when the file can't be read; it is then left untouched. */
ptz_vault *ptz_vault_open(const char *path, char *error, size_t error_size);
void ptz_vault_free(ptz_vault *vault);
ptz_vault_state ptz_vault_get_state(const ptz_vault *vault);
/* Returns 1 once the logins are readable; a wrong password returns 0. */
int ptz_vault_unlock(ptz_vault *vault, const char *admin_password, char *error, size_t error_size);
void ptz_vault_lock(ptz_vault *vault);
/* Creates or changes the Administrator Password and re-encrypts every login, or
 * with NULL or "" removes it and keeps the logins as plain text. Not while locked. */
int ptz_vault_set_admin_password(ptz_vault *vault, const char *admin_password, char *error, size_t error_size);
/* Logins are sorted by name; none are listed while locked. */
size_t ptz_vault_count(const ptz_vault *vault, ptz_store store);
/* The strings stay valid until the vault next changes, locks or is freed. */
int ptz_vault_login(const ptz_vault *vault, ptz_store store, size_t index, ptz_login *login);
/* Replaces the login at `index`, or with index -1 adds one. A login with the same
 * name (ignoring case) is replaced. Writes the file and
 * returns the login's new index, or -1 with `error` filled. */
long ptz_vault_save(ptz_vault *vault, ptz_store store, long index, const ptz_login *login, char *error,
                    size_t error_size);
int ptz_vault_remove(ptz_vault *vault, ptz_store store, size_t index, char *error, size_t error_size);

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
