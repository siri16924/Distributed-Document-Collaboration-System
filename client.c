#include "common.h"
#include <time.h>

void repl(int ns_fd, const char *username);

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ns_ip> <ns_port>\n", argv[0]);
        return 1;
    }
    const char *ns_ip = argv[1];
    int ns_port = atoi(argv[2]);

    char username[MAX_USERNAME];
    printf("Enter username: ");
    fflush(stdout);
    if (!fgets(username, sizeof(username), stdin)) {
        fprintf(stderr, "No username\n");
        return 1;
    }
    username[strcspn(username, "\n")] = '\0';

    int ns_fd = connect_to_server(ns_ip, ns_port);
    if (ns_fd < 0) {
        fprintf(stderr, "Cannot connect to NameServer\n");
        return 1;
    }

    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "REGISTER_CLIENT username=%s", username);
    writeline_fd(ns_fd, buf);
    if (readline_fd(ns_fd, buf, sizeof(buf)) <= 0 || strncmp(buf, "OK", 2) != 0) {
        fprintf(stderr, "Failed to register client: %s\n", buf);
        return 1;
    }

    printf("Connected as '%s'\n", username);
    repl(ns_fd, username);
    close(ns_fd);
    return 0;
}

void handle_view_cmd(int ns_fd, char *cmd) {
    // cmd: "VIEW", "VIEW -a", etc.
    writeline_fd(ns_fd, cmd);
    char line[MAX_LINE];
    while (1) {
        int n = readline_fd(ns_fd, line, sizeof(line));
        if (n <= 0) break;
        if (strcmp(line, "END") == 0) break;
        printf("%s\n", line);
    }
}

void handle_create_cmd(int ns_fd, const char *filename) {
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "CREATE filename=%s", filename);
    writeline_fd(ns_fd, buf);
    if (readline_fd(ns_fd, buf, sizeof(buf)) <= 0) return;
    if (strncmp(buf, "OK", 2) == 0)
        printf("File '%s' created successfully\n", filename);
    else
        printf("%s\n", buf);
}

int get_ss_for_read(int ns_fd, const char *filename, char *ss_ip, int *ss_port) {
    char buf[MAX_LINE];

    // Ask NameServer which SS holds this file
    snprintf(buf, sizeof(buf), "READ_META filename=%s", filename);
    writeline_fd(ns_fd, buf);

    if (readline_fd(ns_fd, buf, sizeof(buf)) <= 0) {
        printf("Error: No response from NameServer\n");
        return -1;
    }

    // Try to parse "OK SS ip=... port=..."
    // If parsing fails, treat the entire line as an error message and print it.
    if (sscanf(buf, "OK SS ip=%63s port=%d", ss_ip, ss_port) != 2) {
        // NameServer sent some error like "ERR NOT_FOUND" or "ERR NO_ACCESS"
        printf("%s\n", buf);
        return -1;
    }

    return 0;
}


void handle_read_cmd(int ns_fd, const char *filename, const char *username) {
    char ss_ip[64];
    int ss_port;
    if (get_ss_for_read(ns_fd, filename, ss_ip, &ss_port) != 0) return;

    int ss_fd = connect_to_server(ss_ip, ss_port);
    if (ss_fd < 0) {
        printf("ERR SS_DOWN\n");
        return;
    }
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "READ filename=%s username=%s", filename, username);
    writeline_fd(ss_fd, buf);

    while (1) {
        int n = readline_fd(ss_fd, buf, sizeof(buf));
        if (n <= 0) break;
        if (strcmp(buf, "END") == 0) break;
        if (strncmp(buf, "ERR", 3) == 0) {
            printf("%s\n", buf);
            break;
        }
        printf("%s\n", buf);
    }
    close(ss_fd);
}

void handle_stream_cmd(int ns_fd, const char *filename, const char *username) {
    char ss_ip[64];
    int ss_port;
    if (get_ss_for_read(ns_fd, filename, ss_ip, &ss_port) != 0) return;

    int ss_fd = connect_to_server(ss_ip, ss_port);
    if (ss_fd < 0) {
        printf("ERR SS_DOWN\n");
        return;
    }
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "STREAM filename=%s username=%s", filename, username);
    writeline_fd(ss_fd, buf);

    while (1) {
        int n = readline_fd(ss_fd, buf, sizeof(buf));
        if (n <= 0) {
            printf("\n[ERROR: storage server went down mid-stream]\n");
            break;
        }
        if (strcmp(buf, "END") == 0) break;
        if (strncmp(buf, "ERR", 3) == 0) {
            printf("%s\n", buf);
            break;
        }
        printf("%s ", buf);
        fflush(stdout);
    }
    printf("\n");
    close(ss_fd);
}

void handle_write_cmd(int ns_fd, const char *filename, int sentence_num, const char *username) {
    char ss_ip[64];
    int ss_port;
    char buf[MAX_LINE];

    // Ask NS which SS to use for this write
    snprintf(buf, sizeof(buf), "WRITE_META filename=%s sentence=%d", filename, sentence_num);
    writeline_fd(ns_fd, buf);

    if (readline_fd(ns_fd, buf, sizeof(buf)) <= 0) {
        printf("Error: No response from nameserver\n");
        return;
    }

    // Try to parse the OK line directly instead of strict strncmp
    if (sscanf(buf, "OK SS ip=%63s port=%d", ss_ip, &ss_port) != 2) {
        // Not in expected format -> treat as error message from NS
        printf("%s\n", buf);
        return;
    }

    // Connect to storage server
    int ss_fd = connect_to_server(ss_ip, ss_port);
    if (ss_fd < 0) {
        printf("ERR SS_DOWN\n");
        return;
    }

    // Start write session on SS
    snprintf(buf, sizeof(buf), "WRITE filename=%s username=%s sentence=%d", filename, username, sentence_num);
    writeline_fd(ss_fd, buf);

    if (readline_fd(ss_fd, buf, sizeof(buf)) <= 0) {
        printf("Error: No response from storage server\n");
        close(ss_fd);
        return;
    }

    if (strncmp(buf, "OK_WRITE_READY", 14) != 0) {
        // SS rejected the write (no access, file not found, etc.)
        printf("%s\n", buf);
        close(ss_fd);
        return;
    }

    printf("Write mode activated. Enter word updates (word_index content) or ETIRW to finish:\n");

    char input_line[MAX_LINE];
    while (fgets(input_line, sizeof(input_line), stdin)) {
        input_line[strcspn(input_line, "\n")] = '\0';

        writeline_fd(ss_fd, input_line);

        if (strcmp(input_line, "ETIRW") == 0) {
            if (readline_fd(ss_fd, buf, sizeof(buf)) > 0) {
                if (strncmp(buf, "OK_WRITE_COMPLETE", 17) == 0) {
                    printf("Write Successful!\n");
                } else {
                    printf("%s\n", buf);
                }
            }
            break;
        } else {
            // optional per-line ACK from SS
            if (readline_fd(ss_fd, buf, sizeof(buf)) > 0) {
                if (strncmp(buf, "OK", 2) != 0) {
                    printf("%s\n", buf);
                }
            }
        }
    }

    close(ss_fd);
}


void handle_delete_cmd(int ns_fd, const char *filename) {
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "DELETE filename=%s", filename);
    writeline_fd(ns_fd, buf);
    
    if (readline_fd(ns_fd, buf, sizeof(buf)) <= 0) return;
    
    if (strncmp(buf, "OK", 2) == 0) {
        printf("File '%s' deleted successfully!\n", filename);
    } else {
        printf("%s\n", buf);
    }
}

void handle_info_cmd(int ns_fd, const char *filename) {
    char buf[MAX_LINE];

    // Ask NameServer directly
    snprintf(buf, sizeof(buf), "INFO filename=%s", filename);
    writeline_fd(ns_fd, buf);

    while (1) {
        int n = readline_fd(ns_fd, buf, sizeof(buf));
        if (n <= 0) return;
        if (strcmp(buf, "END") == 0) break;

        if (strncmp(buf, "ERR", 3) == 0) {
            printf("%s\n", buf);
            return;
        }

        // Pretty print
        printf("--> %s\n", buf);
    }
}




void handle_undo_cmd(int ns_fd, const char *filename) {
    char ss_ip[64];
    int ss_port;
    char buf[MAX_LINE];
    
    if (get_ss_for_read(ns_fd, filename, ss_ip, &ss_port) != 0) return;
    
    int ss_fd = connect_to_server(ss_ip, ss_port);
    if (ss_fd < 0) {
        printf("ERR SS_DOWN\n");
        return;
    }
    
    snprintf(buf, sizeof(buf), "UNDO filename=%s", filename);
    writeline_fd(ss_fd, buf);
    
    if (readline_fd(ss_fd, buf, sizeof(buf)) > 0) {
        if (strncmp(buf, "OK", 2) == 0) {
            printf("Undo Successful!\n");
        } else {
            printf("%s\n", buf);
        }
    }
    
    close(ss_fd);
}

void handle_access_cmd(int ns_fd, const char *cmdline) {
    char buf[MAX_LINE];
    strcpy(buf, cmdline);
    writeline_fd(ns_fd, buf);
    
    if (readline_fd(ns_fd, buf, sizeof(buf)) <= 0) return;
    
    if (strncmp(buf, "OK", 2) == 0) {
        if (strstr(cmdline, "ADDACCESS")) {
            printf("Access granted successfully!\n");
        } else {
            printf("Access removed successfully!\n");
        }
    } else {
        printf("%s\n", buf);
    }
}

void handle_exec_cmd(int ns_fd, const char *filename) {
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "EXEC filename=%s", filename);
    writeline_fd(ns_fd, buf);
    
    int in_output = 0;
    while (readline_fd(ns_fd, buf, sizeof(buf)) > 0) {
        if (strcmp(buf, "EXEC_OUTPUT_END") == 0) break;
        
        if (strcmp(buf, "EXEC_OUTPUT_START") == 0) {
            in_output = 1;
            continue;
        }
        
        if (strncmp(buf, "ERR", 3) == 0) {
            printf("%s\n", buf);
            return;
        }
        
        if (in_output) {
            printf("%s\n", buf);
        }
    }
}


void print_help() {
    printf("Available commands:\n");
    printf("  VIEW [-a] [-l] [-al]     - List files (optional flags: -a=all, -l=detailed)\n");
    printf("  READ <filename>          - Display file content\n");
    printf("  CREATE <filename>        - Create new file\n");
    printf("  WRITE <filename> <sentence_num> - Write to file at sentence index\n");
    printf("  DELETE <filename>        - Delete file (owner only)\n");
    printf("  INFO <filename>          - Display file information\n");
    printf("  UNDO <filename>          - Undo last change to file\n");
    printf("  STREAM <filename>        - Stream file content word by word\n");
    printf("  LIST                     - List all users\n");
    printf("  ADDACCESS -R <filename> <username> - Grant read access\n");
    printf("  ADDACCESS -W <filename> <username> - Grant write access\n");
    printf("  REMACCESS <filename> <username>    - Remove access\n");
    printf("  EXEC <filename>          - Execute file as shell script\n");
    printf("  HELP                     - Show this help\n");
    printf("  QUIT                     - Exit client\n");
}

void repl(int ns_fd, const char *username) {
    char cmdline[MAX_LINE];
    printf("\nWelcome to LangOS File System!\n");
    printf("Type HELP for available commands.\n\n");
    
    while (1) {
        printf("%s> ", username);
        fflush(stdout);
        if (!fgets(cmdline, sizeof(cmdline), stdin)) break;
        cmdline[strcspn(cmdline, "\n")] = '\0';
        if (strlen(cmdline) == 0) continue;

        if (strncmp(cmdline, "VIEW", 4) == 0) {
            handle_view_cmd(ns_fd, cmdline);
        } else if (strncmp(cmdline, "CREATE ", 7) == 0) {
            char fname[MAX_FILENAME];
            if (sscanf(cmdline + 7, "%255s", fname) == 1)
                handle_create_cmd(ns_fd, fname);
        } else if (strncmp(cmdline, "READ ", 5) == 0) {
            char fname[MAX_FILENAME];
            if (sscanf(cmdline + 5, "%255s", fname) == 1)
                handle_read_cmd(ns_fd, fname, username);
        } else if (strncmp(cmdline, "WRITE ", 6) == 0) {
            char fname[MAX_FILENAME];
            int sentence_num = 0; // Default to sentence 0
            int parsed = sscanf(cmdline + 6, "%255s %d", fname, &sentence_num);
            if (parsed >= 1) // At least filename is required
                handle_write_cmd(ns_fd, fname, sentence_num, username);
        } else if (strncmp(cmdline, "DELETE ", 7) == 0) {
            char fname[MAX_FILENAME];
            if (sscanf(cmdline + 7, "%255s", fname) == 1)
                handle_delete_cmd(ns_fd, fname);
        } else if (strncmp(cmdline, "INFO ", 5) == 0) {
            char fname[MAX_FILENAME];
            if (sscanf(cmdline + 5, "%255s", fname) == 1)
                handle_info_cmd(ns_fd, fname);
        } else if (strncmp(cmdline, "UNDO ", 5) == 0) {
            char fname[MAX_FILENAME];
            if (sscanf(cmdline + 5, "%255s", fname) == 1)
                handle_undo_cmd(ns_fd, fname);
        } else if (strncmp(cmdline, "STREAM ", 7) == 0) {
            char fname[MAX_FILENAME];
            if (sscanf(cmdline + 7, "%255s", fname) == 1)
                handle_stream_cmd(ns_fd, fname, username);
        } else if (strncmp(cmdline, "ADDACCESS", 9) == 0 || strncmp(cmdline, "REMACCESS", 9) == 0) {
            handle_access_cmd(ns_fd, cmdline);
        } else if (strncmp(cmdline, "EXEC ", 5) == 0) {
            char fname[MAX_FILENAME];
            if (sscanf(cmdline + 5, "%255s", fname) == 1)
                handle_exec_cmd(ns_fd, fname);
        } else if (strcmp(cmdline, "LIST") == 0) {
            writeline_fd(ns_fd, "LIST_USERS");
            char line[MAX_LINE];
            while (1) {
                int n = readline_fd(ns_fd, line, sizeof(line));
                if (n <= 0) break;
                if (strcmp(line, "END") == 0) break;
                printf("--> %s\n", line);
            }
        } else if (strcmp(cmdline, "HELP") == 0) {
            print_help();
        } else if (strcmp(cmdline, "QUIT") == 0) {
            writeline_fd(ns_fd, "QUIT");
            break;
        } else {
            printf("Unknown command. Type HELP for available commands.\n");
        }
    }
}
