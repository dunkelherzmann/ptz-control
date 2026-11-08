/* Login vault: plain and sealed files, both stores, wrong password, replace,
 * remove, change and removal of the Administrator Password, damaged files. */
#include "ptzcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char buffer[65536];
    size_t n = fread(buffer, 1, sizeof buffer - 1, f);
    fclose(f);
    buffer[n] = 0;
    return buffer;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024], error[256] = "";
    snprintf(path, sizeof path, "%s/logins.vault", dir);
    remove(path);

    ptz_vault *v = ptz_vault_open(path, error, sizeof error);
    CHECK(v && ptz_vault_get_state(v) == PTZ_VAULT_NEW);
    ptz_login ssh = {"Jump.Example.com", 22, "op", "ssh\tsecret\nline", "~/.ssh/id_ed25519"};
    CHECK(ptz_vault_save(v, PTZ_STORE_SSH, -1, &ssh, error, sizeof error) == -1); /* no choice made yet */

    /* Plain text by choice. */
    CHECK(ptz_vault_set_admin_password(v, NULL, error, sizeof error));
    CHECK(ptz_vault_get_state(v) == PTZ_VAULT_PLAIN);
    CHECK(ptz_vault_save(v, PTZ_STORE_SSH, -1, &ssh, error, sizeof error) == 0);
    ptz_login cam1 = {"192.168.1.100", 80, "admin", "cam-one", "ignored"};
    ptz_login cam2 = {"192.168.1.100", 80, "viewer", "cam-two", ""};
    CHECK(ptz_vault_save(v, PTZ_STORE_CAMERA, -1, &cam2, error, sizeof error) == 0);
    CHECK(ptz_vault_save(v, PTZ_STORE_CAMERA, -1, &cam1, error, sizeof error) == 0); /* sorted: admin first */
    CHECK(ptz_vault_count(v, PTZ_STORE_SSH) == 1 && ptz_vault_count(v, PTZ_STORE_CAMERA) == 2);
    ptz_login got;
    CHECK(ptz_vault_login(v, PTZ_STORE_CAMERA, 0, &got) && strcmp(got.user, "admin") == 0 && got.key_file[0] == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(strstr(slurp(path), "cam-one") != NULL);
    ptz_vault_free(v);

    v = ptz_vault_open(path, error, sizeof error);
    CHECK(v && ptz_vault_get_state(v) == PTZ_VAULT_PLAIN && ptz_vault_count(v, PTZ_STORE_CAMERA) == 2);
    CHECK(ptz_vault_login(v, PTZ_STORE_SSH, 0, &got) && strcmp(got.password, "ssh\tsecret\nline") == 0 &&
          strcmp(got.key_file, "~/.ssh/id_ed25519") == 0 && got.port == 22);

    /* Same host (other case), port and user replaces; editing an index moves it. */
    ptz_login ssh2 = {"jump.example.com", 22, "op", "new", ""};
    CHECK(ptz_vault_save(v, PTZ_STORE_SSH, -1, &ssh2, error, sizeof error) == 0);
    CHECK(ptz_vault_count(v, PTZ_STORE_SSH) == 1);
    ptz_login cam3 = {"10.0.0.5", 8080, "admin", "cam-three", ""};
    CHECK(ptz_vault_save(v, PTZ_STORE_CAMERA, 1, &cam3, error, sizeof error) == 0); /* viewer -> 10.0.0.5 */
    CHECK(ptz_vault_count(v, PTZ_STORE_CAMERA) == 2);
    CHECK(ptz_vault_login(v, PTZ_STORE_CAMERA, 1, &got) && strcmp(got.host, "192.168.1.100") == 0 &&
          strcmp(got.user, "admin") == 0);
    ptz_login bad = {"", 22, "op", "", ""};
    CHECK(ptz_vault_save(v, PTZ_STORE_SSH, -1, &bad, error, sizeof error) == -1);

    /* Encrypt: nothing readable on disk, locked on the next open. */
    CHECK(ptz_vault_set_admin_password(v, "test-password", error, sizeof error));
    CHECK(ptz_vault_get_state(v) == PTZ_VAULT_UNLOCKED);
    const char *text = slurp(path);
    CHECK(strncmp(text, "PTZ Control logins 1\nargon2id ", 30) == 0);
    CHECK(!strstr(text, "cam-three") && !strstr(text, "10.0.0.5") && !strstr(text, "jump"));
    ptz_vault_free(v);

    v = ptz_vault_open(path, error, sizeof error);
    CHECK(v && ptz_vault_get_state(v) == PTZ_VAULT_LOCKED && ptz_vault_count(v, PTZ_STORE_SSH) == 0);
    CHECK(ptz_vault_save(v, PTZ_STORE_SSH, -1, &ssh, error, sizeof error) == -1);
    CHECK(!ptz_vault_set_admin_password(v, NULL, error, sizeof error));
    CHECK(!ptz_vault_unlock(v, "wrong", error, sizeof error) && strstr(error, "wrong"));
    CHECK(ptz_vault_get_state(v) == PTZ_VAULT_LOCKED);
    CHECK(ptz_vault_unlock(v, "test-password", error, sizeof error));
    CHECK(ptz_vault_count(v, PTZ_STORE_SSH) == 1 && ptz_vault_count(v, PTZ_STORE_CAMERA) == 2);
    CHECK(ptz_vault_remove(v, PTZ_STORE_CAMERA, 0, error, sizeof error));
    ptz_vault_lock(v);
    CHECK(ptz_vault_get_state(v) == PTZ_VAULT_LOCKED && ptz_vault_count(v, PTZ_STORE_CAMERA) == 0);
    CHECK(ptz_vault_unlock(v, "test-password", error, sizeof error) && ptz_vault_count(v, PTZ_STORE_CAMERA) == 1);

    /* Change the password, then remove it. */
    CHECK(ptz_vault_set_admin_password(v, "second", error, sizeof error));
    ptz_vault_free(v);
    v = ptz_vault_open(path, error, sizeof error);
    CHECK(!ptz_vault_unlock(v, "test-password", error, sizeof error));
    CHECK(ptz_vault_unlock(v, "second", error, sizeof error));
    CHECK(ptz_vault_set_admin_password(v, "", error, sizeof error) && ptz_vault_get_state(v) == PTZ_VAULT_PLAIN);
    CHECK(strstr(slurp(path), "\nplain\n") != NULL);
    ptz_vault_free(v);

    /* Tampered ciphertext and damaged files are refused and left alone. */
    v = ptz_vault_open(path, error, sizeof error);
    CHECK(ptz_vault_set_admin_password(v, "test-password", error, sizeof error));
    ptz_vault_free(v);
    char copy[65536];
    strcpy(copy, slurp(path));
    size_t len = strlen(copy);
    copy[len - 3] = copy[len - 3] == 'a' ? 'b' : 'a';
    FILE *f = fopen(path, "wb");
    fwrite(copy, 1, len, f);
    fclose(f);
    v = ptz_vault_open(path, error, sizeof error);
    CHECK(v && !ptz_vault_unlock(v, "test-password", error, sizeof error));
    ptz_vault_free(v);
    f = fopen(path, "wb");
    fputs("something else\n", f);
    fclose(f);
    CHECK(ptz_vault_open(path, error, sizeof error) == NULL && error[0]);
    CHECK(strcmp(slurp(path), "something else\n") == 0);
    remove(path);

    if (failures) {
        fprintf(stderr, "vault-smoke: %d failure(s)\n", failures);
        return 1;
    }
    puts("vault-smoke: all checks passed");
    return 0;
}
