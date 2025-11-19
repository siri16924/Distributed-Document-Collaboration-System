#include "common.h"
#include <pthread.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <sys/stat.h>

static StorageServerInfo ss_list[MAX_SS];
static int ss_count = 0;

static FileMeta file_table[MAX_FILES];
static int file_count = 0;

static char user_list[MAX_USERS][MAX_USERNAME];
static int user_count = 0;

pthread_mutex_t ns_lock = PTHREAD_MUTEX_INITIALIZER;

void log_msg(const char *fmt, ...) {
    time_t now = time(NULL);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(stdout, "[NS %s] ", tbuf);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    
    fprintf(stdout, "\n");
    fflush(stdout);
}

// find / insert helpers
int find_user(const char *username) {
    for (int i = 0; i < user_count; ++i)
        if (strcmp(user_list[i], username) == 0) return i;
    return -1;
}
int add_user(const char *username) {
    if (user_count >= MAX_USERS) return -1;
    strncpy(user_list[user_count], username, MAX_USERNAME);
    user_list[user_count][MAX_USERNAME-1] = '\0';
    return user_count++;
}

int find_file(const char *fname) {
    for (int i = 0; i < file_count; ++i)
        if (strcmp(file_table[i].filename, fname) == 0) return i;
    return -1;
}

int add_file(const char *fname, const char *owner, int ss_id) {
    if (file_count >= MAX_FILES) return -1;
    FileMeta *fm = &file_table[file_count];
    memset(fm, 0, sizeof(*fm));
    strncpy(fm->filename, fname, MAX_FILENAME);
    fm->filename[MAX_FILENAME-1] = '\0';
    strncpy(fm->owner, owner, MAX_USERNAME);
    fm->owner[MAX_USERNAME-1] = '\0';
    fm->created_at = fm->last_modified = fm->last_access = time(NULL);
    strncpy(fm->last_access_user, owner, MAX_USERNAME);
    fm->last_access_user[MAX_USERNAME-1] = '\0';
    fm->acl_count = 1;
    strncpy(fm->acl[0].username, owner, MAX_USERNAME);
    fm->acl[0].username[MAX_USERNAME-1] = '\0';
    fm->acl[0].access_flags = ACCESS_READ | ACCESS_WRITE;
    fm->ss_id = ss_id;
    return file_count++;
}

StorageServerInfo *choose_ss_for_new_file(void) {
    if (ss_count == 0) return NULL;
    // trivial: choose first SS; you can do round-robin etc.
    return &ss_list[0];
}

void handle_client_connection(int cfd);

void handle_register_ss(char *line, int cfd) {
    // format: "REGISTER_SS ip=127.0.0.1 port_nm=8001 port_client=9001"
    if (ss_count >= MAX_SS) {
        writeline_fd(cfd, "ERR too_many_storage_servers");
        return;
    }
    char ip[64] = "127.0.0.1";
    int port_nm = 0, port_client = 0;
    sscanf(line, "REGISTER_SS ip=%63s port_nm=%d port_client=%d", ip, &port_nm, &port_client);

    pthread_mutex_lock(&ns_lock);
    StorageServerInfo *ss = &ss_list[ss_count];
    ss->id = ss_count;
    strncpy(ss->ip, ip, sizeof(ss->ip));
    ss->ip[sizeof(ss->ip)-1] = '\0';
    ss->port_nm = port_nm;
    ss->port_client = port_client;
    ss->alive = 1;
    int id = ss_count++;
    pthread_mutex_unlock(&ns_lock);

    char buf[128];
    snprintf(buf, sizeof(buf), "OK SS_ID=%d", id);
    writeline_fd(cfd, buf);
    
    // Now request the file list from this storage server (with retry)
    int ss_fd = -1;
    for (int retry = 0; retry < 3; retry++) {
        ss_fd = connect_to_server(ip, port_nm);
        if (ss_fd >= 0) break;
        usleep(500000); // Wait 500ms before retry
    }
    
    if (ss_fd >= 0) {
        writeline_fd(ss_fd, "GET_FILE_LIST");
        
        // Read the file list and add to nameserver
        char file_info[MAX_LINE];
        pthread_mutex_lock(&ns_lock);
        while (readline_fd(ss_fd, file_info, sizeof(file_info)) > 0) {
            if (strcmp(file_info, "END") == 0) break;
            
            // Parse filename:owner format
            char filename[MAX_FILENAME];
            char owner[MAX_USERNAME] = "unknown";
            
            char *colon = strchr(file_info, ':');
            if (colon) {
                *colon = '\0';
                strncpy(filename, file_info, MAX_FILENAME-1);
                filename[MAX_FILENAME-1] = '\0';
                strncpy(owner, colon+1, MAX_USERNAME-1);
                owner[MAX_USERNAME-1] = '\0';
            } else {
                strncpy(filename, file_info, MAX_FILENAME-1);
                filename[MAX_FILENAME-1] = '\0';
            }
            
            // Check if file already exists in our table
            if (find_file(filename) == -1) {
                add_file(filename, owner, id);
                log_msg("Discovered existing file: %s (owner: %s) on SS %d", filename, owner, id);
            }
        }
        pthread_mutex_unlock(&ns_lock);
        close(ss_fd);
    } else {
        log_msg("Failed to connect to storage server for file discovery");
    }
    
    log_msg("Registered new StorageServer with ID %d", id);
}

void handle_register_client(char *line, int cfd) {
    // format: "REGISTER_CLIENT username=alice"
    char username[MAX_USERNAME] = {0};
    sscanf(line, "REGISTER_CLIENT username=%63s", username);
    pthread_mutex_lock(&ns_lock);
    if (find_user(username) == -1) {
        add_user(username);
    }
    pthread_mutex_unlock(&ns_lock);
    writeline_fd(cfd, "OK");
    log_msg("Registered client");
}

void handle_view(char *line, int cfd, const char *username) {
    // Ex: "VIEW" or "VIEW -a" or "VIEW -al" etc.
    int all = strstr(line, "-a") != NULL;
    int detailed = strstr(line, "-l") != NULL;

    pthread_mutex_lock(&ns_lock);
    
    // Print table header if -l
    if (detailed) {
        writeline_fd(cfd, "---------------------------------------------------------");
        writeline_fd(cfd, "| Filename   | Words | Chars | Last Access Time | Owner |");
        writeline_fd(cfd, "---------------------------------------------------------");
    }
    
    for (int i = 0; i < file_count; ++i) {
        FileMeta *fm = &file_table[i];
        
        if (!all) {
            // Show only files user has access to (owner or in ACL)
            int has_access = 0;
            
            // Check if user is owner
            if (strcmp(fm->owner, username) == 0) {
                has_access = 1;
            } else {
                // Check ACL for read access
                for (int j = 0; j < fm->acl_count; ++j) {
                    if (strcmp(fm->acl[j].username, username) == 0 && 
                        (fm->acl[j].access_flags & ACCESS_READ)) {
                        has_access = 1;
                        break;
                    }
                }
            }
            
            if (!has_access) continue;
        }
        
        if (!detailed) {
            char buf[512];
            snprintf(buf, sizeof(buf), "--> %s", fm->filename);
            writeline_fd(cfd, buf);
        } else {
            // Get updated file statistics from storage server
            StorageServerInfo *ss = &ss_list[fm->ss_id];
            int word_count = fm->word_count;
            int char_count = fm->char_count;
            
            // Try to get fresh stats from storage server
            int ss_fd = connect_to_server(ss->ip, ss->port_nm);
            if (ss_fd >= 0) {
                char buf[512];
                snprintf(buf, sizeof(buf), "GET_FILE_STATS filename=%s", fm->filename);
                writeline_fd(ss_fd, buf);
                
                char resp[MAX_LINE];
                if (readline_fd(ss_fd, resp, sizeof(resp)) > 0) {
                    if (strncmp(resp, "OK", 2) == 0) {
                        sscanf(resp, "OK words=%d chars=%d", &word_count, &char_count);
                        // Update our cached values
                        fm->word_count = word_count;
                        fm->char_count = char_count;
                    }
                }
                close(ss_fd);
            }
            
            char tbuf[64];
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", localtime(&fm->last_access));
            char buf[512];
            snprintf(buf, sizeof(buf), "| %-10s | %-5d | %-5d | %-16s | %-5s |",
                     fm->filename, word_count, char_count, tbuf, fm->owner);
            writeline_fd(cfd, buf);
        }
    }
    
    if (detailed) {
        writeline_fd(cfd, "---------------------------------------------------------");
    }
    
    pthread_mutex_unlock(&ns_lock);
    writeline_fd(cfd, "END"); // sentinel
}

void handle_read_meta(char *line, int cfd, const char *username) {
    // "READ_META filename=wowee.txt"
    char fname[MAX_FILENAME] = {0};
    sscanf(line, "READ_META filename=%255s", fname);

    pthread_mutex_lock(&ns_lock);
    int idx = find_file(fname);
    if (idx == -1) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }
    FileMeta *fm = &file_table[idx];

    // check read access
    int allowed = 0;
    for (int i = 0; i < fm->acl_count; ++i) {
        if (strcmp(fm->acl[i].username, username) == 0 && (fm->acl[i].access_flags & ACCESS_READ)) {
            allowed = 1;
            break;
        }
    }
    if (!allowed && strcmp(fm->owner, username) != 0) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NO_ACCESS");
        return;
    }

    StorageServerInfo *ss = &ss_list[fm->ss_id];
    char buf[256];
    snprintf(buf, sizeof(buf), "OK SS ip=%s port=%d", ss->ip, ss->port_client);
    writeline_fd(cfd, buf);
    pthread_mutex_unlock(&ns_lock);
}

void handle_create(char *line, int cfd, const char *username) {
    // "CREATE filename=mouse.txt"
    char fname[MAX_FILENAME] = {0};
    sscanf(line, "CREATE filename=%255s", fname);

    pthread_mutex_lock(&ns_lock);
    if (find_file(fname) != -1) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR ALREADY_EXISTS");
        return;
    }
    StorageServerInfo *ss = choose_ss_for_new_file();
    if (!ss) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NO_STORAGE_SERVER");
        return;
    }
    int ss_id = ss->id;
    pthread_mutex_unlock(&ns_lock);

    // send command to SS over its NM port
    int ss_fd = connect_to_server(ss->ip, ss->port_nm);
    if (ss_fd < 0) {
        writeline_fd(cfd, "ERR SS_DOWN");
        return;
    }
    char buf[512];
    snprintf(buf, sizeof(buf), "CREATE_FILE filename=%s owner=%s", fname, username);
    writeline_fd(ss_fd, buf);

    char resp[MAX_LINE];
    if (readline_fd(ss_fd, resp, sizeof(resp)) <= 0) {
        close(ss_fd);
        writeline_fd(cfd, "ERR SS_DOWN");
        return;
    }
    close(ss_fd);
    if (strncmp(resp, "OK", 2) == 0) {
        pthread_mutex_lock(&ns_lock);
        add_file(fname, username, ss_id);
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "OK");
    } else {
        writeline_fd(cfd, resp); // propagate error
    }
}

// Check if user has access to file
int check_file_access(const char *filename, const char *username, int required_flags) {
    int idx = find_file(filename);
    if (idx == -1) return 0;
    
    FileMeta *fm = &file_table[idx];
    
    // Owner always has full access
    if (strcmp(fm->owner, username) == 0) return 1;
    
    // Check ACL
    for (int i = 0; i < fm->acl_count; i++) {
        if (strcmp(fm->acl[i].username, username) == 0) {
            return (fm->acl[i].access_flags & required_flags) == required_flags;
        }
    }
    return 0;
}

void handle_delete(char *line, int cfd, const char *username) {
    // "DELETE filename=mouse.txt"
    char fname[MAX_FILENAME] = {0};
    sscanf(line, "DELETE filename=%255s", fname);

    pthread_mutex_lock(&ns_lock);
    int idx = find_file(fname);
    if (idx == -1) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }
    
    FileMeta *fm = &file_table[idx];
    
    // Only owner can delete
    if (strcmp(fm->owner, username) != 0) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NO_ACCESS");
        return;
    }
    
    StorageServerInfo *ss = &ss_list[fm->ss_id];
    pthread_mutex_unlock(&ns_lock);

    // Send delete command to storage server
    int ss_fd = connect_to_server(ss->ip, ss->port_nm);
    if (ss_fd < 0) {
        writeline_fd(cfd, "ERR SS_DOWN");
        return;
    }
    
    char buf[512];
    snprintf(buf, sizeof(buf), "DELETE_FILE filename=%s", fname);
    writeline_fd(ss_fd, buf);

    char resp[MAX_LINE];
    if (readline_fd(ss_fd, resp, sizeof(resp)) <= 0) {
        close(ss_fd);
        writeline_fd(cfd, "ERR SS_DOWN");
        return;
    }
    close(ss_fd);
    
    if (strncmp(resp, "OK", 2) == 0) {
        pthread_mutex_lock(&ns_lock);
        // Remove from file table
        for (int i = idx; i < file_count - 1; i++) {
            file_table[i] = file_table[i + 1];
        }
        file_count--;
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "OK");
        log_msg("File deleted: %s by %s", fname, username);
    } else {
        writeline_fd(cfd, resp);
    }
}

void handle_write_meta(char *line, int cfd, const char *username) {
    char fname[MAX_FILENAME] = {0};
    int sentence_num;
    sscanf(line, "WRITE_META filename=%255s sentence=%d", fname, &sentence_num);

    pthread_mutex_lock(&ns_lock);
    if (!check_file_access(fname, username, ACCESS_WRITE)) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NO_ACCESS");
        return;
    }
    
    int idx = find_file(fname);
    if (idx == -1) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }
    
    FileMeta *fm = &file_table[idx];
    StorageServerInfo *ss = &ss_list[fm->ss_id];

    // update timestamps
    time_t now = time(NULL);
    fm->last_modified = now;
    fm->last_access  = now;
    strncpy(fm->last_access_user, username, MAX_USERNAME);
    fm->last_access_user[MAX_USERNAME - 1] = '\0';

    pthread_mutex_unlock(&ns_lock);

    char buf[256];
    snprintf(buf, sizeof(buf), "OK SS ip=%s port=%d", ss->ip, ss->port_client);
    writeline_fd(cfd, buf);
    
    log_msg("Write access granted for %s to %s (sentence %d)", fname, username, sentence_num);
}


void handle_info(char *line, int cfd, const char *username) {
    // "INFO filename=test1.txt"
    char fname[MAX_FILENAME] = {0};
    sscanf(line, "INFO filename=%255s", fname);

    pthread_mutex_lock(&ns_lock);
    int idx = find_file(fname);
    if (idx == -1) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }

    FileMeta *fm = &file_table[idx];

    // Check read access
    if (!check_file_access(fname, username, ACCESS_READ)) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NO_ACCESS");
        return;
    }

    // ---------- Get fresh stats from StorageServer ----------
    StorageServerInfo *ss = &ss_list[fm->ss_id];
    int word_count = fm->word_count;
    int char_count = fm->char_count;

    int ss_fd = connect_to_server(ss->ip, ss->port_nm);
    if (ss_fd >= 0) {
        char req[256];
        snprintf(req, sizeof(req), "GET_FILE_STATS filename=%s", fm->filename);
        writeline_fd(ss_fd, req);

        char resp[MAX_LINE];
        if (readline_fd(ss_fd, resp, sizeof(resp)) > 0) {
            if (strncmp(resp, "OK", 2) == 0) {
                // e.g. "OK words=8 chars=23"
                sscanf(resp, "OK words=%d chars=%d", &word_count, &char_count);
                fm->word_count = word_count;
                fm->char_count = char_count;
            }
        }
        close(ss_fd);
    }
    // If SS_DOWN or error, we just fall back to whatever is in fm.

    // ---------- Build Access: ... ----------
    char access_buf[512] = "";
    snprintf(access_buf, sizeof(access_buf), "%s (RW)", fm->owner); // owner always RW

    for (int i = 0; i < fm->acl_count; i++) {
        if (strcmp(fm->acl[i].username, fm->owner) == 0) continue;
        size_t len = strlen(access_buf);
        if (len + 16 >= sizeof(access_buf)) break; // avoid overflow

        strcat(access_buf, ", ");
        strcat(access_buf, fm->acl[i].username);
        strcat(access_buf, " (");
        if (fm->acl[i].access_flags & ACCESS_READ)  strcat(access_buf, "R");
        if (fm->acl[i].access_flags & ACCESS_WRITE) strcat(access_buf, "W");
        strcat(access_buf, ")");
    }

    // ---------- Human-readable timestamps ----------
    char created_str[64], modified_str[64], last_access_str[64];
    time_t c = fm->created_at;
    time_t m = fm->last_modified;
    time_t a = fm->last_access;
    strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M:%S", localtime(&c));
    strftime(modified_str, sizeof(modified_str), "%Y-%m-%d %H:%M:%S", localtime(&m));
    strftime(last_access_str, sizeof(last_access_str), "%Y-%m-%d %H:%M:%S", localtime(&a));

    // ---------- Send info as multiple small lines ----------
    char buf[256];

    snprintf(buf, sizeof(buf), "File: %s", fm->filename);
    writeline_fd(cfd, buf);

    snprintf(buf, sizeof(buf), "Owner: %s", fm->owner);
    writeline_fd(cfd, buf);

    // Treat size as char_count bytes (simple but fine for this project)
    snprintf(buf, sizeof(buf), "Size: %d bytes", char_count);
    writeline_fd(cfd, buf);

    snprintf(buf, sizeof(buf), "Words: %d", word_count);
    writeline_fd(cfd, buf);

    snprintf(buf, sizeof(buf), "Characters: %d", char_count);
    writeline_fd(cfd, buf);

    snprintf(buf, sizeof(buf), "Created: %s", created_str);
    writeline_fd(cfd, buf);

    snprintf(buf, sizeof(buf), "Modified: %s", modified_str);
    writeline_fd(cfd, buf);

    snprintf(buf, sizeof(buf), "Access: %s", access_buf);
    writeline_fd(cfd, buf);

    snprintf(buf, sizeof(buf), "Last Access: %s by %s", last_access_str, fm->last_access_user);
    writeline_fd(cfd, buf);

    pthread_mutex_unlock(&ns_lock);

    writeline_fd(cfd, "END");
    log_msg("Info request for %s by %s", fname, username);
}


void handle_addaccess(char *line, int cfd, const char *username) {
    // Client sends: "ADDACCESS -R abc.txt user2"
    //               "ADDACCESS -W abc.txt user2"
    char flag[8];
    char fname[MAX_FILENAME] = {0};
    char target_user[MAX_USERNAME] = {0};

    if (sscanf(line, "ADDACCESS %7s %255s %63s", flag, fname, target_user) != 3) {
        writeline_fd(cfd, "ERR INVALID_ADDACCESS_FORMAT");
        return;
    }

    int is_write = (strcmp(flag, "-W") == 0);

    pthread_mutex_lock(&ns_lock);
    int idx = find_file(fname);
    if (idx == -1) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }

    FileMeta *fm = &file_table[idx];

    // Only owner can modify access
    if (strcmp(fm->owner, username) != 0) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NO_ACCESS");
        return;
    }

    // Find existing ACL entry for target user
    int acl_idx = -1;
    for (int i = 0; i < fm->acl_count; i++) {
        if (strcmp(fm->acl[i].username, target_user) == 0) {
            acl_idx = i;
            break;
        }
    }

    if (acl_idx == -1) {
        // Add new ACL entry
        if (fm->acl_count >= 32) {
            pthread_mutex_unlock(&ns_lock);
            writeline_fd(cfd, "ERR ACL_FULL");
            return;
        }
        acl_idx = fm->acl_count++;
        strncpy(fm->acl[acl_idx].username, target_user, MAX_USERNAME - 1);
        fm->acl[acl_idx].username[MAX_USERNAME - 1] = '\0';
        fm->acl[acl_idx].access_flags = 0;
    }

    // Set access flags
    if (is_write) {
        fm->acl[acl_idx].access_flags = ACCESS_READ | ACCESS_WRITE;
    } else {
        fm->acl[acl_idx].access_flags |= ACCESS_READ;
    }

    pthread_mutex_unlock(&ns_lock);
    writeline_fd(cfd, "OK");

    log_msg("Access %s granted to %s for file %s by %s",
            is_write ? "WRITE" : "READ", target_user, fname, username);
}

void handle_remaccess(char *line, int cfd, const char *username) {
    // Client sends: "REMACCESS abc.txt user2"
    char fname[MAX_FILENAME] = {0};
    char target_user[MAX_USERNAME] = {0};

    if (sscanf(line, "REMACCESS %255s %63s", fname, target_user) != 2) {
        writeline_fd(cfd, "ERR INVALID_REMACCESS_FORMAT");
        return;
    }

    pthread_mutex_lock(&ns_lock);
    int idx = find_file(fname);
    if (idx == -1) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }

    FileMeta *fm = &file_table[idx];

    // Only owner can modify access
    if (strcmp(fm->owner, username) != 0) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NO_ACCESS");
        return;
    }

    // Do not allow removing owner’s own access
    if (strcmp(target_user, fm->owner) == 0) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR CANNOT_REMOVE_OWNER");
        return;
    }

    // Find and remove ACL entry
    int found = 0;
    for (int i = 0; i < fm->acl_count; i++) {
        if (strcmp(fm->acl[i].username, target_user) == 0) {
            for (int j = i; j < fm->acl_count - 1; j++) {
                fm->acl[j] = fm->acl[j + 1];
            }
            fm->acl_count--;
            found = 1;
            break;
        }
    }

    pthread_mutex_unlock(&ns_lock);

    if (found) {
        writeline_fd(cfd, "OK");
        log_msg("Access removed for %s from file %s by %s",
                target_user, fname, username);
    } else {
        writeline_fd(cfd, "ERR NOT_FOUND");
    }
}


void handle_exec(char *line, int cfd, const char *username) {
    // "EXEC filename=script.txt"
    char fname[MAX_FILENAME] = {0};
    sscanf(line, "EXEC filename=%255s", fname);

    pthread_mutex_lock(&ns_lock);
    if (!check_file_access(fname, username, ACCESS_READ)) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NO_ACCESS");
        return;
    }
    
    int idx = find_file(fname);
    if (idx == -1) {
        pthread_mutex_unlock(&ns_lock);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }
    
    FileMeta *fm = &file_table[idx];
    StorageServerInfo *ss = &ss_list[fm->ss_id];
    pthread_mutex_unlock(&ns_lock);

    // First, get file content from storage server
    int ss_fd = connect_to_server(ss->ip, ss->port_client);
    if (ss_fd < 0) {
        writeline_fd(cfd, "ERR SS_DOWN");
        return;
    }
    
    char buf[512];
    snprintf(buf, sizeof(buf), "READ filename=%s username=%s", fname, username);
    writeline_fd(ss_fd, buf);
    
    // Read file content
    char file_content[8192] = "";
    char content_line[MAX_LINE];
    while (readline_fd(ss_fd, content_line, sizeof(content_line)) > 0) {
        if (strcmp(content_line, "END") == 0) break;
        if (strncmp(content_line, "ERR", 3) == 0) {
            close(ss_fd);
            writeline_fd(cfd, content_line);
            return;
        }
        strcat(file_content, content_line);
        strcat(file_content, "\n");
    }
    close(ss_fd);
    
    // Execute the commands
    writeline_fd(cfd, "EXEC_OUTPUT_START");
    
    // Create a temporary script file
    char temp_script[] = "/tmp/ns_exec_XXXXXX";
    int temp_fd = mkstemp(temp_script);
    if (temp_fd != -1) {
        write(temp_fd, file_content, strlen(file_content));
        close(temp_fd);
        chmod(temp_script, 0755);
        
        // Execute the script and capture output
        FILE *exec_pipe = popen(temp_script, "r");
        if (exec_pipe) {
            char output_line[1024];
            while (fgets(output_line, sizeof(output_line), exec_pipe)) {
                output_line[strcspn(output_line, "\n")] = '\0';
                writeline_fd(cfd, output_line);
            }
            pclose(exec_pipe);
        }
        
        unlink(temp_script);
    }
    
    writeline_fd(cfd, "EXEC_OUTPUT_END");
    log_msg("Executed file %s for user %s", fname, username);
}

void handle_client_connection(int cfd_arg) {
    int cfd = cfd_arg;
    char line[MAX_LINE];
    char username[MAX_USERNAME] = "UNKNOWN";

    // First message must be either REGISTER_SS or REGISTER_CLIENT
    if (readline_fd(cfd, line, sizeof(line)) <= 0) {
        close(cfd);
        return;
    }

    if (strncmp(line, "REGISTER_SS", 11) == 0) {
        handle_register_ss(line, cfd);
        close(cfd);
        return;
    } else if (strncmp(line, "REGISTER_CLIENT", 15) == 0) {
        // store username
        sscanf(line, "REGISTER_CLIENT username=%63s", username);
        handle_register_client(line, cfd);
    } else {
        writeline_fd(cfd, "ERR INVALID first_command");
        close(cfd);
        return;
    }

    // After registration, handle commands in loop
    while (1) {
        int n = readline_fd(cfd, line, sizeof(line));
        if (n <= 0) break;

        log_msg("Processing command: %s from user: %s", line, username);
        
        if (strncmp(line, "VIEW", 4) == 0) {
            handle_view(line, cfd, username);
        } else if (strncmp(line, "READ_META", 9) == 0) {
            handle_read_meta(line, cfd, username);
        } else if (strncmp(line, "WRITE_META", 10) == 0) {
            handle_write_meta(line, cfd, username);
        } else if (strncmp(line, "CREATE ", 7) == 0) {
            handle_create(line, cfd, username);
        } else if (strncmp(line, "DELETE ", 7) == 0) {
            handle_delete(line, cfd, username);
        } else if (strncmp(line, "WRITE_META", 10) == 0) {
            handle_write_meta(line, cfd, username);
        } else if (strncmp(line, "INFO ", 5) == 0) {
            handle_info(line, cfd, username);
        } else if (strncmp(line, "ADDACCESS", 9) == 0) {
            handle_addaccess(line, cfd, username);
        } else if (strncmp(line, "REMACCESS", 9) == 0) {
            handle_remaccess(line, cfd, username);
        } else if (strncmp(line, "EXEC ", 5) == 0) {
            handle_exec(line, cfd, username);
        } else if (strcmp(line, "LIST") == 0 || strcmp(line, "LIST_USERS") == 0) {
            pthread_mutex_lock(&ns_lock);
            for (int i = 0; i < user_count; ++i) {
                writeline_fd(cfd, user_list[i]);
            }
            pthread_mutex_unlock(&ns_lock);
            writeline_fd(cfd, "END");
        } else if (strcmp(line, "QUIT") == 0) {
            break;
        } else {
            writeline_fd(cfd, "ERR UNKNOWN_COMMAND");
        }
    }

    close(cfd);
    log_msg("Client disconnected");
}

void *client_thread(void *arg) {
    int cfd = *(int *)arg;
    free(arg);
    handle_client_connection(cfd);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ns_port>\n", argv[0]);
        exit(1);
    }
    int ns_port = atoi(argv[1]);
    int listen_fd = create_server_socket(ns_port);
    log_msg("NameServer started");

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int cfd = accept(listen_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (cfd < 0) { perror("accept"); continue; }

        pthread_t tid;
        int *arg = malloc(sizeof(int));
        *arg = cfd;
        pthread_create(&tid, NULL, client_thread, arg);
        pthread_detach(tid);
    }
    return 0;
}
