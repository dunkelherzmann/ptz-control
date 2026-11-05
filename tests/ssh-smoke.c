/* SSH-route check: tunnel + askpass relay against a throwaway sshd and the mock
 * camera. Run through scripts/test-ssh.sh, which sets everything up. */
#define _XOPEN_SOURCE 700  /* realpath and PATH_MAX under strict C11 on glibc */
#include "ptzcore.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int prompts = 0;

static void on_log(void *context, const char *line) { (void)context; printf("      log: %s\n", line); }

static int on_prompt(void *context, const char *message, int secret, char *answer, size_t size) {
    (void)context;
    prompts++;
    printf("      prompt (%s): %s\n", secret ? "secret" : "plain", message);
    snprintf(answer, size, "%s", getenv("PTZ_TEST_KEY_PASSPHRASE"));
    return 1;
}

static int on_confirm(void *context, const char *title, const char *message) {
    (void)context;
    printf("      confirm: %s — %s\n", title, message);
    return 0;
}

int main(int argc, char **argv) {
    if (ptz_is_askpass_invocation()) return ptz_askpass_main(argc, argv);
    char self[PATH_MAX];
    if (!realpath(argv[0], self)) return 2;

    ptz_connect_params p = {0};
    p.route = PTZ_ROUTE_SSH;
    p.vendor = PTZ_VENDOR_ONVIF;
    p.ssh_host = "127.0.0.1";
    p.ssh_port = atoi(getenv("PTZ_TEST_SSH_PORT"));
    p.ssh_user = getenv("USER");
    p.ssh_password = "";
    p.ssh_identity = getenv("PTZ_TEST_KEY");
    p.askpass_path = self;
    p.camera_host = "127.0.0.1";
    p.camera_port = 8080;
    p.camera_user = "admin";
    p.camera_password = "secret";
    p.https_port = 8443;
    p.timeout = 10;
    ptz_callbacks cb = {NULL, on_log, on_prompt, on_confirm};
    char error[512] = {0};
    ptz_session *s = ptz_connect(&p, &cb, NULL, error, sizeof error);
    if (!s) {
        printf("FAIL: ssh connect: %s\n", error);
        return 1;
    }
    printf("ok:   connected through SSH tunnel after %d prompt(s)\n", prompts);
    int failures = prompts < 1;
    if (prompts < 1) printf("FAIL: expected the key passphrase prompt to reach the host\n");

    ptz_position pos;
    if (ptz_get_position(s, &pos, error, sizeof error)) printf("ok:   position through tunnel pan=%s\n", pos.pan.text);
    else { printf("FAIL: position: %s\n", error); failures++; }
    ptz_target t = {1, 1, 0, -0.25, 0.5, 0};
    if (ptz_move(s, &t, error, sizeof error)) printf("ok:   move through tunnel\n");
    else { printf("FAIL: move: %s\n", error); failures++; }
    char url[512], notice[256];
    if (ptz_web_url(s, url, sizeof url, notice, sizeof notice, error, sizeof error)) printf("ok:   web url %s\n", url);
    else { printf("FAIL: web: %s\n", error); failures++; }
    printf("ok:   link alive=%d\n", ptz_link_alive(s));
    ptz_close(s);
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
