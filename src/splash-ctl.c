/*
 * splash-ctl.c - JSON client for splash-drm daemon
 * 
 * Usage: splash-ctl [options] <json-string>
 *        splash-ctl [options] --file <json-file>
 * 
 * The JSON can be a single command object or an array of commands.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCKET_NAME "splash-drm"

static int debug_mode = 0;
static int raw_mode = 0;

static int send_json(const char *json_str) {
    if (debug_mode) {
        fprintf(stderr, "[debug] Sending: %s\n", json_str);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, SOCKET_NAME, sizeof(addr.sun_path) - 2);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to splash-drm daemon: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    size_t len = strlen(json_str);
    if (write(fd, json_str, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
        fprintf(stderr, "Failed to write command\n");
        close(fd);
        return -1;
    }

    /* Read response */
    char reply[4096];
    int n = read(fd, reply, sizeof(reply) - 1);
    if (n > 0) {
        reply[n] = '\0';
        if (raw_mode || debug_mode) {
            printf("%s\n", reply);
        } else {
            /* Pretty-print basic status */
            if (strstr(reply, "\"status\":\"ok\"")) {
                printf("OK\n");
            } else if (strstr(reply, "\"status\":\"error\"")) {
                printf("ERROR\n");
                char *msg = strstr(reply, "\"message\":\"");
                if (msg) {
                    msg += 11;
                    char *end = strchr(msg, '\"');
                    if (end) {
                        printf("  ");
                        fwrite(msg, 1, end - msg, stdout);
                        printf("\n");
                    }
                }
            } else {
                printf("%s\n", reply);
            }
        }
    }

    close(fd);
    return 0;
}

static char* read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "splash-ctl v3.0 - JSON client for splash-drm daemon\n\n"
        "Usage: %s [options] <json-string>\n"
        "       %s [options] --file <json-file>\n\n"
        "Options:\n"
        "  --debug    Show debug output\n"
        "  --raw      Output raw JSON response\n"
        "  --file     Read JSON from file instead of argument\n\n"
        "JSON format (single command):\n"
        "  {\"cmd\": \"text\", \"id\": 0, \"x\": 100, \"y\": 200, \"text\": \"Hello\"}\n\n"
        "JSON format (batch):\n"
        "  [{\"cmd\": \"clear\"}, {\"cmd\": \"image\", \"path\": \"splash.png\"}]\n\n"
        "Examples:\n"
        "  %s '{\"cmd\":\"suspend\"}'\n"
        "  %s '{\"cmd\":\"text\",\"id\":0,\"x\":100,\"y\":200,\"text\":\"Hello\"}'\n"
        "  %s '[{\"cmd\":\"clear\"},{\"cmd\":\"image\",\"path\":\"boot.png\"}]'\n"
        "  %s --file /etc/splash-commands.json\n"
        "  %s '{\"cmd\":\"status\"}'\n"
        "  %s '{\"cmd\":\"resume\"}'\n"
        "  %s '{\"cmd\":\"exit\"}'\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    int arg_offset = 1;
    int use_file = 0;

    while (arg_offset < argc && argv[arg_offset][0] == '-') {
        if (strcmp(argv[arg_offset], "--debug") == 0) {
            debug_mode = 1;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--raw") == 0) {
            raw_mode = 1;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--file") == 0) {
            use_file = 1;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--help") == 0 || strcmp(argv[arg_offset], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[arg_offset]);
            return 1;
        }
    }

    if (arg_offset >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    char *json_str = NULL;
    int needs_free = 0;

    if (use_file) {
        json_str = read_file(argv[arg_offset]);
        if (!json_str) return 1;
        needs_free = 1;
    } else {
        /* Concatenate all remaining arguments */
        size_t total = 0;
        for (int i = arg_offset; i < argc; i++) {
            total += strlen(argv[i]) + 1;
        }
        json_str = malloc(total + 1);
        if (!json_str) {
            fprintf(stderr, "Out of memory\n");
            return 1;
        }
        needs_free = 1;
        
        char *p = json_str;
        for (int i = arg_offset; i < argc; i++) {
            if (i > arg_offset) *p++ = ' ';
            strcpy(p, argv[i]);
            p += strlen(argv[i]);
        }
        *p = '\0';
    }

    /* Validate JSON */
    if (json_str[0] != '{' && json_str[0] != '[') {
        fprintf(stderr, "Error: Argument does not look like JSON (must start with { or [)\n");
        if (needs_free) free(json_str);
        return 1;
    }

    int ret = send_json(json_str);
    if (needs_free) free(json_str);
    return ret;
}
