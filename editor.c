#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    int size;
    char *buf;
} EditorRow;

typedef enum { NORMAL, INSERT } Mode;

typedef struct {
    int cx, cy;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int num_rows;
    EditorRow *editor_rows;
    int dirty;
    char *filename;
    Mode mode;
    char statusmsg[80];
} EditorState;

static EditorState E;
static struct termios original_termios;

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN
};

void clear_screen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

void kys(const char *s) {
    clear_screen();
    perror(s);
    exit(1);
}

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &original_termios) == -1) kys("tcgetattr");
    atexit(disable_raw_mode);

    struct termios raw = original_termios;
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) kys("tcsetattr");
}

int editor_read_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) kys("read");
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                }
            }
        }
        return '\x1b';
    }
    return c;
}

void get_term_size() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0)
        kys("ioctl");
    E.screenrows = ws.ws_row - 1;
    E.screencols = ws.ws_col;
}

void editor_insert_row(int at, const char *s, size_t len) {
    if (at < 0 || at > E.num_rows) return;

    E.editor_rows =
        realloc(E.editor_rows, sizeof(EditorRow) * (E.num_rows + 1));
    memmove(&E.editor_rows[at + 1], &E.editor_rows[at],
            sizeof(EditorRow) * (E.num_rows - at));

    E.editor_rows[at].size = len;
    E.editor_rows[at].buf = malloc(len + 1);
    memcpy(E.editor_rows[at].buf, s, len);
    E.editor_rows[at].buf[len] = '\0';
    E.num_rows++;
    E.dirty++;
}

void editor_free_row(EditorRow *row) { free(row->buf); }

void editor_del_row(int at) {
    if (at < 0 || at >= E.num_rows) return;
    editor_free_row(&E.editor_rows[at]);
    memmove(&E.editor_rows[at], &E.editor_rows[at + 1],
            sizeof(EditorRow) * (E.num_rows - at - 1));
    E.num_rows--;
    E.dirty++;
}

void editor_row_insert_char(EditorRow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->buf = realloc(row->buf, row->size + 2);
    memmove(&row->buf[at + 1], &row->buf[at], row->size - at + 1);
    row->size++;
    row->buf[at] = c;
    E.dirty++;
}

void editor_row_append_string(EditorRow *row, char *s, size_t len) {
    row->buf = realloc(row->buf, row->size + len + 1);
    memcpy(&row->buf[row->size], s, len);
    row->size += len;
    row->buf[row->size] = '\0';
    E.dirty++;
}

void editor_row_del_char(EditorRow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->buf[at], &row->buf[at + 1], row->size - at);
    row->size--;
    E.dirty++;
}

void editor_insert_char(int c) {
    if (E.cy == E.num_rows) {
        editor_insert_row(E.num_rows, "", 0);
    }
    editor_row_insert_char(&E.editor_rows[E.cy], E.cx, c);
    E.cx++;
}

void editor_insert_newline() {
    if (E.cx == 0) {
        editor_insert_row(E.cy, "", 0);
    } else {
        EditorRow *row = &E.editor_rows[E.cy];
        editor_insert_row(E.cy + 1, &row->buf[E.cx], row->size - E.cx);
        row = &E.editor_rows[E.cy];
        row->size = E.cx;
        row->buf[row->size] = '\0';
    }
    E.cy++;
    E.cx = 0;
}

void editor_del_char() {
    if (E.cy == E.num_rows || (E.cx == 0 && E.cy == 0)) return;

    EditorRow *row = &E.editor_rows[E.cy];
    if (E.cx > 0) {
        editor_row_del_char(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.editor_rows[E.cy - 1].size;
        editor_row_append_string(&E.editor_rows[E.cy - 1], row->buf,
                                 row->size);
        editor_del_row(E.cy);
        E.cy--;
    }
}

void editor_open(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) return;

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 &&
               (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            linelen--;
        }
        editor_insert_row(E.num_rows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editor_save() {
    if (E.filename == NULL) return;

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, 0) != -1) {
            for (int i = 0; i < E.num_rows; i++) {
                write(fd, E.editor_rows[i].buf, E.editor_rows[i].size);
                write(fd, "\n", 1);
            }
            close(fd);
            E.dirty = 0;
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                     "saved %s successfully.", E.filename);
            return;
        }
        close(fd);
    }
    snprintf(E.statusmsg, sizeof(E.statusmsg), "save failed!");
}

typedef struct {
    char *buf;
    int length;
} Abuf;

void ab_append(Abuf *ab, const char *s, int length) {
    char *new_buf = realloc(ab->buf, ab->length + length);
    if (!new_buf) return;
    memcpy(new_buf + ab->length, s, length);
    ab->buf = new_buf;
    ab->length += length;
}

void ab_free(Abuf *ab) { free(ab->buf); }

void editor_scroll() {
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows)
        E.rowoff = E.cy - E.screenrows + 1;
    if (E.cx < E.coloff) E.coloff = E.cx;
    if (E.cx >= E.coloff + E.screencols)
        E.coloff = E.cx - E.screencols + 1;
}

void draw_rows(Abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.num_rows) {
            ab_append(ab, "\x1b[33m*\x1b[39m", 11);
        } else {
            int len = E.editor_rows[filerow].size - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            ab_append(ab, &E.editor_rows[filerow].buf[E.coloff], len);
        }
        ab_append(ab, "\x1b[K", 3);
        ab_append(ab, "\r\n", 2);
    }
}

void draw_status_bar(Abuf *ab) {
    ab_append(ab, "\x1b[44m", 5);
    char status[80], rstatus[80];

    int len = snprintf(status, sizeof(status), "%s \"%.20s\" %dL %s",
                       E.mode == NORMAL ? "NORMAL" : "INSERT",
                       E.filename ? E.filename : "[no name]", E.num_rows,
                       E.dirty ? "dirty" : "");

    int rlen = snprintf(rstatus, sizeof(rstatus), "%d:%d ", E.cy + 1,
                        E.cx + 1);

    if (len > E.screencols) len = E.screencols;
    ab_append(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            ab_append(ab, rstatus, rlen);
            break;
        } else {
            ab_append(ab, " ", 1);
            len++;
        }
    }
    ab_append(ab, "\x1b[m", 3);
}

void refresh_screen() {
    editor_scroll();
    Abuf ab = {NULL, 0};

    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    draw_rows(&ab);
    draw_status_bar(&ab);

    // correct placement of the cursor
    char seq[32];
    snprintf(seq, sizeof(seq), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
             (E.cx - E.coloff) + 1);
    ab_append(&ab, seq, strlen(seq));

    ab_append(&ab, "\x1b[?25h", 6); // cursor
    write(STDOUT_FILENO, ab.buf, ab.length);
    ab_free(&ab);
}

void move_cursor(int key) {
    EditorRow *row = (E.cy >= E.num_rows) ? NULL : &E.editor_rows[E.cy];

    switch (key) {
    case ARROW_LEFT:
    case 'h':
        if (E.cx > 0) E.cx--;
        break;
    case ARROW_RIGHT:
    case 'l':
        if (row && E.cx < row->size) E.cx++;
        break;
    case ARROW_UP:
    case 'k':
        if (E.cy > 0) E.cy--;
        break;
    case ARROW_DOWN:
    case 'j':
        if (E.cy < E.num_rows - 1) E.cy++;
        break;
    }

    row = (E.cy >= E.num_rows) ? NULL : &E.editor_rows[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

void process_keypress() {
    int c = editor_read_key();

    switch (c) {
    case PAGE_UP:
    case PAGE_DOWN: {
        int times = E.screenrows;
        while (times--)
            move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        return;
    }
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT: move_cursor(c); return;
    }

    if (E.mode == NORMAL) {
        switch (c) {
        case 'q':
            clear_screen();
            exit(0);
            break;
        case 'w': editor_save(); break;
        case 'i': E.mode = INSERT; break;
        case 'h':
        case 'j':
        case 'k':
        case 'l': move_cursor(c); break;
        }
    } else {
        switch (c) {
        case 27: E.mode = NORMAL; break;
        case '\r': editor_insert_newline(); break;
        case BACKSPACE:
        case 8: // Ctrl-H
            editor_del_char();
            break;
        default:
            if (!iscntrl(c) && c < 128) {
                editor_insert_char(c);
            }
            break;
        }
    }
}

void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.num_rows = 0;
    E.editor_rows = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.mode = NORMAL;
    E.statusmsg[0] = '\0';
    get_term_size();
}

int main(int argc, char *argv[]) {
    enable_raw_mode();
    init_editor();

    if (argc >= 2) {
        editor_open(argv[1]);
    } else {
        editor_insert_row(0, "", 0);
    }

    while (1) {
        refresh_screen();
        process_keypress();
    }

    return 0;
}
