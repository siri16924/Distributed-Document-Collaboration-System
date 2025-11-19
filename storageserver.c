#include "common.h"
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>

static char base_dir[256] = "./data/ss1";
static int ss_id = -1;

pthread_mutex_t ss_lock = PTHREAD_MUTEX_INITIALIZER;

// File lock structure for sentence-level locking
typedef struct {
    char filename[MAX_FILENAME];
    int sentence_num;
    char locked_by[MAX_USERNAME];
    time_t lock_time;
} SentenceLock;

static SentenceLock sentence_locks[256];  // Max 256 concurrent sentence locks
static int num_locks = 0;
pthread_mutex_t lock_mutex = PTHREAD_MUTEX_INITIALIZER;

// Undo history structure
typedef struct {
    char filename[MAX_FILENAME];
    char backup_content[8192];  // Store previous version
    time_t undo_time;
} UndoHistory;

static UndoHistory undo_history[MAX_FILES];
static int undo_count = 0;
pthread_mutex_t undo_mutex = PTHREAD_MUTEX_INITIALIZER;

// Load entire file into a malloc'd buffer (caller must free). Returns NULL on error.
char *load_file_to_string(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

// Save string to file, overwriting.
int save_string_to_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    if (fputs(data, f) == EOF) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

void ss_log(const char *fmt, ...) {
    time_t now = time(NULL);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(stdout, "[SS %s] ", tbuf);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    
    fprintf(stdout, "\n");
    fflush(stdout);
}

void filepath_for(const char *filename, char *buf, size_t len) {
    snprintf(buf, len, "%s/%s", base_dir, filename);
}

void metadata_filepath_for(const char *filename, char *buf, size_t len) {
    snprintf(buf, len, "%s/.meta_%s", base_dir, filename);
}

// Helper function to count words and characters in a file
void count_file_stats(const char *filepath, int *word_count, int *char_count) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        *word_count = *char_count = 0;
        return;
    }
    
    *word_count = 0;
    *char_count = 0;
    char word[256];
    
    while (fscanf(f, "%255s", word) == 1) {
        (*word_count)++;
        *char_count += strlen(word);
    }
    
    fclose(f);
}

int parse_sentences(const char *content, char sentences[][1024], int max_sentences) {
    int count = 0;
    char cur[1024];
    int cur_len = 0;

    size_t len = strlen(content);

    for (size_t i = 0; i < len; i++) {
        char c = content[i];

        if (cur_len < (int)sizeof(cur) - 1) {
            cur[cur_len++] = c;
        }

        if (c == '.' || c == '!' || c == '?') {
            cur[cur_len] = '\0';

            int start = 0;
            while (isspace((unsigned char)cur[start])) start++;
            int end = cur_len - 1;
            while (end >= start && isspace((unsigned char)cur[end])) end--;

            if (end >= start && count < max_sentences) {
                int out_len = end - start + 1;
                strncpy(sentences[count], cur + start, out_len);
                sentences[count][out_len] = '\0';
                count++;
            }
            cur_len = 0;
        }
    }

    // trailing chunk w/o delimiter is still a sentence
    if (cur_len > 0 && count < max_sentences) {
        cur[cur_len] = '\0';
        int start = 0;
        while (isspace((unsigned char)cur[start])) start++;
        int end = cur_len - 1;
        while (end >= start && isspace((unsigned char)cur[end])) end--;

        if (end >= start) {
            int out_len = end - start + 1;
            strncpy(sentences[count], cur + start, out_len);
            sentences[count][out_len] = '\0';
            count++;
        }
    }

    return count;
}


// Check if sentence is locked
int is_sentence_locked(const char *filename, int sentence_num, const char *username) {
    pthread_mutex_lock(&lock_mutex);
    for (int i = 0; i < num_locks; i++) {
        if (strcmp(sentence_locks[i].filename, filename) == 0 && 
            sentence_locks[i].sentence_num == sentence_num) {
            if (strcmp(sentence_locks[i].locked_by, username) != 0) {
                pthread_mutex_unlock(&lock_mutex);
                return 1; // Locked by another user
            }
            break;
        }
    }
    pthread_mutex_unlock(&lock_mutex);
    return 0; // Not locked or locked by same user
}

// Lock a sentence
int lock_sentence(const char *filename, int sentence_num, const char *username) {
    pthread_mutex_lock(&lock_mutex);
    
    // Check if already locked by someone else
    for (int i = 0; i < num_locks; i++) {
        if (strcmp(sentence_locks[i].filename, filename) == 0 && 
            sentence_locks[i].sentence_num == sentence_num) {
            if (strcmp(sentence_locks[i].locked_by, username) != 0) {
                pthread_mutex_unlock(&lock_mutex);
                return -1; // Already locked by another user
            }
            // Already locked by same user, just update time
            sentence_locks[i].lock_time = time(NULL);
            pthread_mutex_unlock(&lock_mutex);
            return 0;
        }
    }
    
    // Add new lock
    if (num_locks >= 256) {
        pthread_mutex_unlock(&lock_mutex);
        return -1; // Too many locks
    }
    
    strcpy(sentence_locks[num_locks].filename, filename);
    sentence_locks[num_locks].sentence_num = sentence_num;
    strcpy(sentence_locks[num_locks].locked_by, username);
    sentence_locks[num_locks].lock_time = time(NULL);
    num_locks++;
    
    pthread_mutex_unlock(&lock_mutex);
    return 0;
}

// Unlock a sentence
void unlock_sentence(const char *filename, int sentence_num, const char *username) {
    pthread_mutex_lock(&lock_mutex);
    for (int i = 0; i < num_locks; i++) {
        if (strcmp(sentence_locks[i].filename, filename) == 0 && 
            sentence_locks[i].sentence_num == sentence_num &&
            strcmp(sentence_locks[i].locked_by, username) == 0) {
            // Remove lock by shifting remaining locks
            for (int j = i; j < num_locks - 1; j++) {
                sentence_locks[j] = sentence_locks[j + 1];
            }
            num_locks--;
            break;
        }
    }
    pthread_mutex_unlock(&lock_mutex);
}

// Save undo history
void save_undo_history(const char *filename) {
    char filepath[512];
    filepath_for(filename, filepath, sizeof(filepath));
    
    FILE *f = fopen(filepath, "r");
    if (!f) return;
    
    pthread_mutex_lock(&undo_mutex);
    
    // Find existing entry or create new one
    int idx = -1;
    for (int i = 0; i < undo_count; i++) {
        if (strcmp(undo_history[i].filename, filename) == 0) {
            idx = i;
            break;
        }
    }
    
    if (idx == -1 && undo_count < MAX_FILES) {
        idx = undo_count++;
        strcpy(undo_history[idx].filename, filename);
    }
    
    if (idx != -1) {
        // Read current content into backup
        fread(undo_history[idx].backup_content, 1, sizeof(undo_history[idx].backup_content) - 1, f);
        undo_history[idx].backup_content[sizeof(undo_history[idx].backup_content) - 1] = '\0';
        undo_history[idx].undo_time = time(NULL);
    }
    
    fclose(f);
    pthread_mutex_unlock(&undo_mutex);
}

// ---- Handle NM connection ----

void handle_nm_command(int cfd) {
    char line[MAX_LINE];
    while (1) {
        int n = readline_fd(cfd, line, sizeof(line));
        if (n <= 0) break;

        ss_log("Received NM command: %s", line);

        if (strncmp(line, "CREATE_FILE", 11) == 0) {
            char fname[MAX_FILENAME], owner[MAX_USERNAME];
            sscanf(line, "CREATE_FILE filename=%255s owner=%63s", fname, owner);

            char path[512], meta_path[512];
            filepath_for(fname, path, sizeof(path));
            metadata_filepath_for(fname, meta_path, sizeof(meta_path));

            pthread_mutex_lock(&ss_lock);
            FILE *f = fopen(path, "r");
            if (f != NULL) {
                fclose(f);
                pthread_mutex_unlock(&ss_lock);
                writeline_fd(cfd, "ERR ALREADY_EXISTS");
                continue;
            }
            
            // Create empty file
            f = fopen(path, "w");
            if (!f) {
                pthread_mutex_unlock(&ss_lock);
                writeline_fd(cfd, "ERR INTERNAL");
                continue;
            }
            fclose(f);
            
            // Create metadata file
            FILE *meta_f = fopen(meta_path, "w");
            if (meta_f) {
                time_t now = time(NULL);
                fprintf(meta_f, "filename=%s\n", fname);
                fprintf(meta_f, "owner=%s\n", owner);
                fprintf(meta_f, "created=%ld\n", now);
                fprintf(meta_f, "modified=%ld\n", now);
                fprintf(meta_f, "accessed=%ld\n", now);
                fprintf(meta_f, "access_user=%s\n", owner);
                fprintf(meta_f, "word_count=0\n");
                fprintf(meta_f, "char_count=0\n");
                fprintf(meta_f, "acl_count=1\n");
                fprintf(meta_f, "acl_0=%s:RW\n", owner);
                fclose(meta_f);
            }
            
            pthread_mutex_unlock(&ss_lock);
            writeline_fd(cfd, "OK");
            
        } else if (strncmp(line, "DELETE_FILE", 11) == 0) {
            char fname[MAX_FILENAME];
            sscanf(line, "DELETE_FILE filename=%255s", fname);

            char path[512], meta_path[512];
            filepath_for(fname, path, sizeof(path));
            metadata_filepath_for(fname, meta_path, sizeof(meta_path));

            pthread_mutex_lock(&ss_lock);
            if (unlink(path) == 0) {
                unlink(meta_path); // Remove metadata too
                writeline_fd(cfd, "OK");
            } else {
                writeline_fd(cfd, "ERR NOT_FOUND");
            }
            pthread_mutex_unlock(&ss_lock);
            
        } else if (strncmp(line, "GET_FILE_LIST", 13) == 0) {
            pthread_mutex_lock(&ss_lock);
            DIR *dir = opendir(base_dir);
            if (dir) {
                struct dirent *entry;
                while ((entry = readdir(dir)) != NULL) {
                    // Skip . and .. directories and metadata files
                    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 && strncmp(entry->d_name, ".meta_", 6) != 0) {
                        // Check if it's a regular file
                        struct stat file_stat;
                        char full_path[512];
                        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, entry->d_name);
                        if (stat(full_path, &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
                            // Try to read metadata file to get owner info
                            char meta_path[512];
                            metadata_filepath_for(entry->d_name, meta_path, sizeof(meta_path));
                            
                            char owner[MAX_USERNAME] = "unknown";
                            FILE *meta_f = fopen(meta_path, "r");
                            if (meta_f) {
                                char meta_line[512];
                                while (fgets(meta_line, sizeof(meta_line), meta_f)) {
                                    if (strncmp(meta_line, "owner=", 6) == 0) {
                                        sscanf(meta_line, "owner=%63s", owner);
                                        break;
                                    }
                                }
                                fclose(meta_f);
                            } else {
                                // Create basic metadata file for files without metadata
                                meta_f = fopen(meta_path, "w");
                                if (meta_f) {
                                    time_t now = time(NULL);
                                    strcpy(owner, "admin"); // Default owner for existing files
                                    fprintf(meta_f, "filename=%s\n", entry->d_name);
                                    fprintf(meta_f, "owner=%s\n", owner);
                                    fprintf(meta_f, "created=%ld\n", now);
                                    fprintf(meta_f, "modified=%ld\n", now);
                                    fprintf(meta_f, "accessed=%ld\n", now);
                                    fprintf(meta_f, "access_user=%s\n", owner);
                                    fprintf(meta_f, "word_count=0\n");
                                    fprintf(meta_f, "char_count=0\n");
                                    fprintf(meta_f, "acl_count=1\n");
                                    fprintf(meta_f, "acl_0=%s:RW\n", owner);
                                    fclose(meta_f);
                                }
                            }
                            
                            // Send filename:owner format
                            char file_info[512];
                            snprintf(file_info, sizeof(file_info), "%s:%s", entry->d_name, owner);
                            writeline_fd(cfd, file_info);
                        }
                    }
                }
                closedir(dir);
            }
            writeline_fd(cfd, "END");
            pthread_mutex_unlock(&ss_lock);
            
        } else if (strncmp(line, "GET_FILE_STATS", 14) == 0) {
            // "GET_FILE_STATS filename=test.txt"
            char fname[MAX_FILENAME] = {0};
            sscanf(line, "GET_FILE_STATS filename=%255s", fname);
            
            pthread_mutex_lock(&ss_lock);
            
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", base_dir, fname);
            
            // Check if file exists
            struct stat file_stat;
            if (stat(filepath, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
                writeline_fd(cfd, "ERR NOT_FOUND");
                pthread_mutex_unlock(&ss_lock);
            } else {
                // Count words and characters
                int word_count, char_count;
                count_file_stats(filepath, &word_count, &char_count);
                
                char buf[256];
                snprintf(buf, sizeof(buf), "OK words=%d chars=%d", word_count, char_count);
                writeline_fd(cfd, buf);
                pthread_mutex_unlock(&ss_lock);
            }
            
        } else {
            writeline_fd(cfd, "ERR UNKNOWN_COMMAND");
        }
    }
    close(cfd);
}

void *nm_handler(void *arg) {
    int listen_fd = *(int *)arg;
    free(arg);
    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int cfd = accept(listen_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (cfd < 0) { perror("accept nm"); continue; }

        handle_nm_command(cfd);
    }
    return NULL;
}

// ---- Handle client connection ----

void handle_client_read(int cfd, const char *line) {
    // Accept both:
    //   "READ filename=wowee.txt"
    //   "READ filename=wowee.txt username=user1"
    char fname[MAX_FILENAME];
    char username[MAX_USERNAME] = "unknown";

    int matched = sscanf(line, "READ filename=%255s username=%63s", fname, username);
    if (matched < 1) {
        writeline_fd(cfd, "ERR INVALID_READ");
        return;
    }

    char path[512];
    filepath_for(fname, path, sizeof(path));

    pthread_mutex_lock(&ss_lock);
    FILE *f = fopen(path, "r");
    if (!f) {
        pthread_mutex_unlock(&ss_lock);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }

    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = '\0';
        writeline_fd(cfd, buf);
    }
    fclose(f);

    // Update access time in metadata
    char meta_path[512];
    metadata_filepath_for(fname, meta_path, sizeof(meta_path));
    FILE *meta_f = fopen(meta_path, "a");
    if (meta_f) {
        fprintf(meta_f, "accessed=%ld\n", time(NULL));
        fprintf(meta_f, "access_user=%s\n", username);
        fclose(meta_f);
    }

    pthread_mutex_unlock(&ss_lock);
    writeline_fd(cfd, "END");
}

void handle_client_stream(int cfd, const char *line) {
    // "STREAM filename=wowee.txt username=user1"
    char fname[MAX_FILENAME], username[MAX_USERNAME] = "unknown";
    sscanf(line, "STREAM filename=%255s username=%63s", fname, username);
    
    char path[512];
    filepath_for(fname, path, sizeof(path));

    pthread_mutex_lock(&ss_lock);
    FILE *f = fopen(path, "r");
    if (!f) {
        pthread_mutex_unlock(&ss_lock);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }
    
    // Update access time in metadata
    char meta_path[512];
    metadata_filepath_for(fname, meta_path, sizeof(meta_path));
    FILE *meta_f = fopen(meta_path, "a");
    if (meta_f) {
        fprintf(meta_f, "accessed=%ld\n", time(NULL));
        fprintf(meta_f, "access_user=%s\n", username);
        fclose(meta_f);
    }
    
    char word[256];
    while (fscanf(f, "%255s", word) == 1) {
        writeline_fd(cfd, word);
        pthread_mutex_unlock(&ss_lock); // let others run while sleeping
        usleep(100000); // 0.1 seconds
        pthread_mutex_lock(&ss_lock);
    }
    fclose(f);
    pthread_mutex_unlock(&ss_lock);
    writeline_fd(cfd, "END");
}

void handle_client_write(int cfd, const char *line) {
    // "WRITE filename=test.txt username=user1 sentence=0"
    char fname[MAX_FILENAME], username[MAX_USERNAME] = "unknown";
    int sentence_num;

    sscanf(line, "WRITE filename=%255s username=%63s sentence=%d",
           fname, username, &sentence_num);

    ss_log("Write request: file=%s, user=%s, sentence=%d",
           fname, username, sentence_num);

    // basic sanity
    if (sentence_num < 0) {
        writeline_fd(cfd, "ERR Sentence index out of range");
        return;
    }

    // Check lock
    if (is_sentence_locked(fname, sentence_num, username)) {
        writeline_fd(cfd, "ERR LOCKED");
        return;
    }
    if (lock_sentence(fname, sentence_num, username) != 0) {
        writeline_fd(cfd, "ERR LOCKED");
        return;
    }

    char path[512];
    filepath_for(fname, path, sizeof(path));

    pthread_mutex_lock(&ss_lock);

    // Read current file content
    FILE *f = fopen(path, "r");
    if (!f) {
        pthread_mutex_unlock(&ss_lock);
        unlock_sentence(fname, sentence_num, username);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }

    // Save for UNDO
    save_undo_history(fname);

    char content[8192] = "";
    size_t nread = fread(content, 1, sizeof(content) - 1, f);
    content[nread] = '\0';
    fclose(f);

    // Parse into sentences
    char sentences[256][1024];
    int num_sentences = parse_sentences(content, sentences, 256);

    ss_log("Parsed %d sentences for %s", num_sentences, fname);

    // ---- Sentence index rules ----
    // 1) Empty file: only sentence_num == 0 is allowed
    //    -> we create the first empty sentence buffer.
    // 2) Non-empty file: legal indices are 0 .. num_sentences-1 only.
    if (num_sentences == 0) {
        if (sentence_num != 0) {
            pthread_mutex_unlock(&ss_lock);
            unlock_sentence(fname, sentence_num, username);
            writeline_fd(cfd, "ERR Sentence index out of range");
            return;
        }
        // create first empty sentence
        strcpy(sentences[0], "");
        num_sentences = 1;
    } else {
        if (sentence_num >= num_sentences) {
            pthread_mutex_unlock(&ss_lock);
            unlock_sentence(fname, sentence_num, username);
            writeline_fd(cfd, "ERR Sentence index out of range");
            return;
        }
    }

    pthread_mutex_unlock(&ss_lock);

    // Tell client we’re ready to receive word updates
    writeline_fd(cfd, "OK_WRITE_READY");

    // ---------- Handle updates loop ----------
    char update_line[MAX_LINE];

    while (1) {
        int n = readline_fd(cfd, update_line, sizeof(update_line));
        if (n <= 0) break;

        if (strcmp(update_line, "ETIRW") == 0) {
            // Finalize write: rebuild full content and write back
            pthread_mutex_lock(&ss_lock);

            FILE *wf = fopen(path, "w");
            if (wf) {
                // rebuild content from sentences[]
                for (int i = 0; i < num_sentences; i++) {
                    fputs(sentences[i], wf);
                    if (i < num_sentences - 1) fputc(' ', wf);
                }
                fclose(wf);
            }

            // (optional) update metadata file here if you like

            pthread_mutex_unlock(&ss_lock);
            unlock_sentence(fname, sentence_num, username);
            writeline_fd(cfd, "OK_WRITE_COMPLETE");
            break;
        } else {
            // Expect: "<word_index> <content...>"
            int word_index;
            char new_content[512];

            if (sscanf(update_line, "%d %511[^\n]", &word_index, new_content) != 2) {
                writeline_fd(cfd, "ERR INVALID_UPDATE");
                continue;
            }

            if (word_index < 0) {
                writeline_fd(cfd, "ERR Word index out of range");
                continue;
            }

            // Split target sentence into words
            char words[128][64];
            int word_count = 0;

            char *sentence_copy = strdup(sentences[sentence_num]);
            char *tok = strtok(sentence_copy, " ");
            while (tok && word_count < 128) {
                strncpy(words[word_count], tok, sizeof(words[word_count]) - 1);
                words[word_count][sizeof(words[word_count]) - 1] = '\0';
                word_count++;
                tok = strtok(NULL, " ");
            }

            if (word_index > word_count + 1) {
                free(sentence_copy);
                writeline_fd(cfd, "ERR Word index out of range");
                continue;
            }

            // Insert at position word_index
            if (word_index <= word_count) {
                // shift right
                for (int i = word_count; i > word_index; i--) {
                    strcpy(words[i], words[i - 1]);
                }
                strncpy(words[word_index], new_content,
                        sizeof(words[word_index]) - 1);
                words[word_index][sizeof(words[word_index]) - 1] = '\0';
                word_count++;
            }

            // Rebuild that sentence string
            sentences[sentence_num][0] = '\0';
            for (int i = 0; i < word_count; i++) {
                if (i > 0) strcat(sentences[sentence_num], " ");
                strcat(sentences[sentence_num], words[i]);
            }

            free(sentence_copy);

            // If new_content contained sentence delimiters, we could
            // re-parse the entire file here into sentences[] again.
            // (You already have that logic; keep it if you need it.)

            writeline_fd(cfd, "OK");
        }
    }
}

void handle_client_undo(int cfd, const char *line) {
    // "UNDO filename=test.txt"
    char fname[MAX_FILENAME];
    sscanf(line, "UNDO filename=%255s", fname);
    
    pthread_mutex_lock(&undo_mutex);
    
    int found = -1;
    for (int i = 0; i < undo_count; i++) {
        if (strcmp(undo_history[i].filename, fname) == 0) {
            found = i;
            break;
        }
    }
    
    if (found == -1) {
        pthread_mutex_unlock(&undo_mutex);
        writeline_fd(cfd, "ERR NO_UNDO_HISTORY");
        return;
    }
    
    char path[512];
    filepath_for(fname, path, sizeof(path));
    
    pthread_mutex_lock(&ss_lock);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s", undo_history[found].backup_content);
        fclose(f);
        
        // Update metadata
        char meta_path[512];
        metadata_filepath_for(fname, meta_path, sizeof(meta_path));
        FILE *meta_f = fopen(meta_path, "a");
        if (meta_f) {
            fprintf(meta_f, "modified=%ld\n", time(NULL));
            fclose(meta_f);
        }
        
        writeline_fd(cfd, "OK");
    } else {
        writeline_fd(cfd, "ERR INTERNAL");
    }
    pthread_mutex_unlock(&ss_lock);
    
    // Remove undo history entry
    for (int i = found; i < undo_count - 1; i++) {
        undo_history[i] = undo_history[i + 1];
    }
    undo_count--;
    
    pthread_mutex_unlock(&undo_mutex);
}

void handle_client_info(int cfd, const char *line) {
    // "INFO filename=test.txt"
    char fname[MAX_FILENAME];
    sscanf(line, "INFO filename=%255s", fname);
    
    char path[512], meta_path[512];
    filepath_for(fname, path, sizeof(path));
    metadata_filepath_for(fname, meta_path, sizeof(meta_path));
    
    pthread_mutex_lock(&ss_lock);
    
    // Get file stats
    struct stat file_stat;
    if (stat(path, &file_stat) != 0) {
        pthread_mutex_unlock(&ss_lock);
        writeline_fd(cfd, "ERR NOT_FOUND");
        return;
    }
    
    // Count words and characters
    int word_count, char_count;
    count_file_stats(path, &word_count, &char_count);
    
    // Read metadata
    char owner[MAX_USERNAME] = "unknown";
    time_t created = file_stat.st_ctime;
    time_t modified = file_stat.st_mtime;
    time_t accessed = file_stat.st_atime;
    char access_user[MAX_USERNAME] = "unknown";
    
    FILE *meta_f = fopen(meta_path, "r");
    if (meta_f) {
        char meta_line[512];
        while (fgets(meta_line, sizeof(meta_line), meta_f)) {
            if (strncmp(meta_line, "owner=", 6) == 0) {
                sscanf(meta_line, "owner=%63s", owner);
            } else if (strncmp(meta_line, "created=", 8) == 0) {
                sscanf(meta_line, "created=%ld", &created);
            } else if (strncmp(meta_line, "modified=", 9) == 0) {
                sscanf(meta_line, "modified=%ld", &modified);
            } else if (strncmp(meta_line, "access_user=", 12) == 0) {
                sscanf(meta_line, "access_user=%63s", access_user);
            }
        }
        fclose(meta_f);
    }
    
    pthread_mutex_unlock(&ss_lock);
    
    // Send file info
    char info_buf[1024];
    snprintf(info_buf, sizeof(info_buf), 
        "File: %s|Owner: %s|Size: %ld bytes|Words: %d|Characters: %d|Created: %ld|Modified: %ld|Last Access: %ld by %s",
        fname, owner, file_stat.st_size, word_count, char_count, created, modified, accessed, access_user);
    writeline_fd(cfd, info_buf);
}

void handle_client_conn(int cfd) {
    char line[MAX_LINE];
    while (1) {
        int n = readline_fd(cfd, line, sizeof(line));
        if (n <= 0) break;
        
        ss_log("Client command: %s", line);
        
        if (strncmp(line, "READ ", 5) == 0) {
            handle_client_read(cfd, line);
        } else if (strncmp(line, "STREAM ", 7) == 0) {
            handle_client_stream(cfd, line);
        } else if (strncmp(line, "WRITE ", 6) == 0) {
            handle_client_write(cfd, line);
        } else if (strncmp(line, "UNDO ", 5) == 0) {
            handle_client_undo(cfd, line);
        } else if (strncmp(line, "INFO ", 5) == 0) {
            handle_client_info(cfd, line);
        } else if (strcmp(line, "QUIT") == 0) {
            break;
        } else {
            writeline_fd(cfd, "ERR UNKNOWN_COMMAND");
        }
    }
    close(cfd);
}

void *client_connection_handler(void *arg) {
    int cfd = *(int *)arg;
    free(arg);
    handle_client_conn(cfd);
    return NULL;
}

void *client_handler(void *arg) {
    int listen_fd = *(int *)arg;
    free(arg);
    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int cfd = accept(listen_fd, (struct sockaddr *)&cliaddr, &clilen);
        if (cfd < 0) { perror("accept client"); continue; }

        pthread_t tid;
        int *fd_arg = malloc(sizeof(int));
        *fd_arg = cfd;
        if (pthread_create(&tid, NULL, client_connection_handler, fd_arg) != 0) {
            perror("pthread_create");
            close(cfd);
            free(fd_arg);
            continue;
        }
        pthread_detach(tid);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <ns_ip> <ns_port> <port_nm> <port_client> <base_dir>\n", argv[0]);
        exit(1);
    }
    const char *ns_ip = argv[1];
    int ns_port = atoi(argv[2]);
    int port_nm = atoi(argv[3]);
    int port_client = atoi(argv[4]);
    strncpy(base_dir, argv[5], sizeof(base_dir));
    base_dir[sizeof(base_dir)-1] = '\0';

    // ensure base_dir exists
    mkdir(base_dir, 0777);

    ss_log("StorageServer starting...");

    // Connect to NS and register
    int ns_fd = connect_to_server(ns_ip, ns_port);
    if (ns_fd < 0) {
        fprintf(stderr, "Cannot connect to NameServer\n");
        exit(1);
    }
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "REGISTER_SS ip=127.0.0.1 port_nm=%d port_client=%d", port_nm, port_client);
    writeline_fd(ns_fd, buf);
    if (readline_fd(ns_fd, buf, sizeof(buf)) <= 0) {
        fprintf(stderr, "No response from NameServer\n");
        exit(1);
    }
    if (strncmp(buf, "OK SS_ID=", 9) == 0) {
        sscanf(buf, "OK SS_ID=%d", &ss_id);
        ss_log("Registered with NameServer");
    } else {
        fprintf(stderr, "Failed registration: %s\n", buf);
        exit(1);
    }
    close(ns_fd);

    // Start listeners
    int nm_listen_fd = create_server_socket(port_nm);
    int client_listen_fd = create_server_socket(port_client);

    pthread_t t1, t2;
    int *a1 = malloc(sizeof(int));
    *a1 = nm_listen_fd;
    pthread_create(&t1, NULL, nm_handler, a1);

    int *a2 = malloc(sizeof(int));
    *a2 = client_listen_fd;
    pthread_create(&t2, NULL, client_handler, a2);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
