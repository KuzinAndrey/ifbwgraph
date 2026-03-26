/*
ifbwgraph - High resolution network interfaces bandwidth graphs

CVS: https://github.com/KuzinAndrey/ifbwgraph
Author: Kuzin Andrey <kuzinandrey@yandex.ru>

History:
  2026-03-20 - Initial public release
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <pthread.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/times.h>
#include <signal.h>

#include <evhttp.h>
#include <event2/http.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>

#include <png.h>
#include "tinyfont.h"

#define COLOR_BG 0
#define COLOR_WHITE 1
#define COLOR_BLACK 2
#define COLOR_GREEN 3
#define COLOR_BLUE 4
#define COLOR_RED 5
#define COLOR_GRAY 6
#define COLOR_ORANGE 7

// Options
static size_t opt_graph_seconds = 600; // 10 minutes
static const char *opt_listening_address = "127.0.0.1";
static int opt_server_port = 8080;
static const char *opt_description_file = NULL;

// Global state
static volatile int program_state = 0;
struct event_base *base;
struct evhttp *http;
struct evhttp_bound_socket *handle;
pthread_t thread_id;

// Iface stats
struct iface_stat {
    // List mutexed fields
    char dev[IF_NAMESIZE + 1];
    struct iface_stat *next;

    // Local mutexed data
    pthread_mutex_t data_mutex;
    char *description;
    size_t cursor;
    time_t cursor_time;
    int mark_for_delete;
    uint64_t rx_bytes, tx_bytes;
    uint64_t overflow_64bit;
    uint32_t *rx_b;
    uint32_t *tx_b;
};

static struct iface_stat *iface_stat_list = NULL;
static pthread_rwlock_t iface_stat_list_rwlock = PTHREAD_RWLOCK_INITIALIZER;

// PNG Image palette
png_color palette[] = {
    [COLOR_BG] = {0, 0, 0},
    [COLOR_WHITE] = {255, 255, 255},
    [COLOR_BLACK] = {0, 0, 0},
    [COLOR_RED] = {255, 0, 0},
    [COLOR_GREEN] = {0, 255, 0},
    [COLOR_BLUE] = {0, 0, 255},
    [COLOR_GRAY] = { 200, 200, 200},
    [COLOR_ORANGE] = {255, 200, 0},
};

png_byte alpha[] = {
    [COLOR_BG] = 0, // fully transparent color
    [COLOR_WHITE] = 255,
    [COLOR_BLACK] = 255,
    [COLOR_RED] = 255,
    [COLOR_GREEN] = 255,
    [COLOR_BLUE] = 255,
    [COLOR_GRAY] = 255,
    [COLOR_ORANGE] = 255,
};

// File IO wrappers to push PNG data to evbuffer
void png_write_to_evbuffer(png_structp png_ptr,
    png_bytep data, png_size_t length)
{
    struct evbuffer *buf = (struct evbuffer *)png_get_io_ptr(png_ptr);
    evbuffer_add(buf, data, length);
}

void png_flush_nothing(png_structp png_ptr) {
    (void)png_ptr;
}

// Generate char on image by font bitmap
void put_char(png_bytep img, png_uint_32 w, png_uint_32 h,
    char *font, int fw, int fh,
    png_uint_32 px, png_uint_32 py,
    char color, char ch)
{
    int offset = ch * fh; // bits encoding
    // int offset = ch * fh * fw; // bytes encoding
    for (png_uint_32 j = 0; j < fh; j++) {
        for (png_uint_32 i = 0; i < fw; i++) {
            char pixel = (font[offset + j] >> (fw - i)) & 0x01;
            //char pixel = font[offset + j * fw + i];
            int x = px + i;
            int y = py + j;
            if (pixel && (x >= 0) && (x < w) && (y >= 0) && (y < h))
                img[y * w + x] = color;
        }
    }
}

// Generate string on image
void put_str(png_bytep img, png_uint_32 w, png_uint_32 h,
    char *font, int fw, int fh,
    png_uint_32 px, png_uint_32 py,
    char color, char *str)
{
    int x = px;
    char *ch = str;
    while (*ch) {
        put_char(img, w, h, font, fw, fh, x, py, color, *ch);
        x += fw;
        ch++;
    }
} // put_str()

// Vertical line
void vline(png_bytep img, png_uint_32 w, png_uint_32 h,
    png_uint_32 x, png_int_32 y1, png_int_32 y2,
    char color)
{
    size_t start, end;

    if (y2 < y1) { // swap
        y1 = y1 ^ y2;
        y2 = y1 ^ y2;
        y1 = y1 ^ y2;
    }
    if (y1 > h || y2 < 0) return;

    if (y1 < 0) y1 = 0;
    if (y2 > h) y2 = h;

    start  = y1 * w + x;
    end = y2 * w + x;
    while (start < end) {
        img[start] = color;
        start += w;
    }
} // vline()

// Generate iface bandwidth graph
void generate_iface_graph(png_bytep img,
    png_uint_32 width, png_uint_32 height,
    struct iface_stat *s)
{
    png_uint_32
        left_x = 60, right_x = width - 20,
        top_y = 3, bottom_y = height - 30,
        y1, y2, o1, gr_height = bottom_y - top_y;

    uint32_t max_val = 0;
    uint32_t levels[5] = {0};
    time_t now = time(NULL);
    time_t start = now - opt_graph_seconds + 60; // graph start time

    // Calculate levels
    for (int i = 0; i < opt_graph_seconds; i++) {
        if (max_val < s->rx_b[i]) max_val = s->rx_b[i];
        if (max_val < s->tx_b[i]) max_val = s->tx_b[i];
    }
    max_val = max_val * 8 / 1000; // in kbit
    if (max_val < 8) max_val = 8;
    for (int i = 1; i < sizeof(levels)/sizeof(levels[0]) - 1; i++) {
        levels[i] = (max_val / 4) * i;
    }
    levels[4] = max_val;

    // Fill image with background color
    memset(img, COLOR_WHITE, width * height);

    // Main iface histogram
    time_t ct = now;
    for (int x = opt_graph_seconds; x > 0; x--, ct--) {
        if (ct == s->cursor_time) {
            float rx = s->rx_b[s->cursor] * 8 / 1000.0 / max_val;
            float tx = s->tx_b[s->cursor] * 8 / 1000.0 / max_val;
            if (rx > tx) {
                vline(img, width, height, left_x + x, bottom_y - (int)(rx * gr_height),
                      bottom_y - (int)(tx * gr_height), COLOR_GREEN);
                vline(img, width, height, left_x + x, bottom_y - (int)(tx * gr_height),
                      bottom_y, COLOR_BLUE);
            } else {
                vline(img, width, height, left_x + x, bottom_y - (int)(tx * gr_height),
                      bottom_y - (int)(rx * gr_height), COLOR_BLUE);
                vline(img, width, height, left_x + x, bottom_y - (int)(rx * gr_height),
                      bottom_y, COLOR_GREEN);
            }

            s->cursor_time--;
            if (s->cursor == 0) s->cursor = opt_graph_seconds; else s->cursor--;
        }
    }

    // IN, OUT labels
    y1 = bottom_y + 15;
    y2 = bottom_y + 15 + 10;
    o1 = (y1 - 1) * width + left_x - 1;
    memset(img + o1, COLOR_BLACK, 12);
    memset(img + o1 + 100, COLOR_BLACK, 12);
    for (png_uint_32 y = y1; y < y2; y++) {
        o1 = y * width + left_x;
        memset(img + o1, COLOR_GREEN, 10);
        memset(img + o1 + 100, COLOR_BLUE, 10);
        img[o1 - 1] = COLOR_BLACK;
        img[o1 + 10] = COLOR_BLACK;
        img[o1 + 100 - 1] = COLOR_BLACK;
        img[o1 + 100 + 10] = COLOR_BLACK;
    }
    o1 = y2 * width + left_x - 1;
    memset(img + o1, COLOR_BLACK, 12);
    memset(img + o1 + 100, COLOR_BLACK, 12);
    put_str(img, width, height, tinyfont, tinyfont_width, tinyfont_height,
            left_x + 14, bottom_y + 16, COLOR_BLACK, "- IN traffic");
    put_str(img, width, height, tinyfont, tinyfont_width, tinyfont_height,
            left_x + 100 + 14, bottom_y + 16, COLOR_BLACK, "- OUT traffic");

    // Graph border
    memset(img + top_y * width + left_x, COLOR_BLACK, right_x - left_x);
    memset(img + bottom_y * width + left_x, COLOR_BLACK, right_x - left_x);
    for (png_uint_32 y = top_y; y < bottom_y; y++) {
        img[y * width + left_x] = COLOR_BLACK;
        img[y * width + right_x] = COLOR_BLACK;
    }

    // Graph horizontal levels
    for (png_uint_32 n = 1; n < 4; n++) {
        png_uint_32 ys = (top_y + n * (bottom_y - top_y) / 4) * width;
        for (png_uint_32 x = left_x - 2; x < right_x; (x > left_x ? x+=2 : x++)) {
            img[ys + x] = COLOR_BLACK;
        }
    }

    // Speed levels text
    for (int n = 0; n < 5; n++) {
        char speed[20];
        sprintf(speed, "%u kb", levels[n]);
        put_str(img, width, height, tinyfont, tinyfont_width, tinyfont_height,
                left_x - tinyfont_width * strlen(speed) - 3,
                bottom_y - ((bottom_y - top_y) / 4) * n - tinyfont_height/2,
                COLOR_BLACK, speed);
    };

    // Graph vertical minutes
    struct tm tval = {0};
    localtime_r(&start, &tval);
    int first_sec = 60 - tval.tm_sec;
    int cur_min = tval.tm_min;
    int cur_hour = tval.tm_hour;
    char timestr[6]; // HH:MM\0
    y1 = bottom_y - 2;
    y2 = bottom_y + 3;
    for (png_uint_32 y = top_y; y < y2; (y < y1 ? y += 2 : y++)) {
        png_uint_32 ys = y * width;
        for (png_uint_32 x = left_x + first_sec; x < right_x; x += 60) {
            img[ys + x] = COLOR_BLACK;
        }
    }

    // Graph time text
    for (png_uint_32 x = left_x + first_sec; x <= right_x; x += 60) {
        sprintf(timestr, "%02d:%02d", cur_hour, cur_min);
        put_str(img, width, height, tinyfont, tinyfont_width, tinyfont_height,
                x - (int)(tinyfont_width * 2.5), bottom_y + 3, COLOR_BLACK, timestr);
        cur_min++;
        if (cur_min == 60) { cur_min = 0; cur_hour++; };
        if (cur_hour == 24) { cur_hour = 0; };
    }

    return;
} // generate_iface_graph()

// WWW "/" handler
int www_index_handler(struct evhttp_request *req, struct evbuffer *reply_buf) {
    struct iface_stat *t;
    FILE *f = NULL;
    char f_name[100];
    long int speed = 0;

    if (!req || !reply_buf) return HTTP_INTERNAL;

    evbuffer_add_printf(reply_buf, "<html>\n");
    evbuffer_add_printf(reply_buf, "<head><title>ifbwgraph: Ifaces Bandwidth</title></head>\n");
    evbuffer_add_printf(reply_buf, "<body bgcolor=white>\n");
    pthread_rwlock_rdlock(&iface_stat_list_rwlock);
    for (t = iface_stat_list; t; t = t->next) {
        if (t->mark_for_delete) continue;
        evbuffer_add_printf(reply_buf, "<p>%s", t->dev);

        if (t->description) {
            evbuffer_add_printf(reply_buf, " [%s]", t->description);
        }

        sprintf(f_name, "/sys/class/net/%s/operstate", t->dev);
        if (NULL != (f = fopen(f_name, "r"))) {
            if (fscanf(f, "%s", f_name) == 1) {
                evbuffer_add_printf(reply_buf, ", %s", f_name);
            };
            fclose(f);
        };

        sprintf(f_name, "/sys/class/net/%s/speed", t->dev);
        if (NULL != (f = fopen(f_name, "r"))) {
            if (fscanf(f, "%ld", &speed) == 1 && speed > 0) {
                t->overflow_64bit = ~(uint64_t)0 - (speed * 1000000) / 8;
                evbuffer_add_printf(reply_buf, ", %ld Mbps", speed);
            };
            fclose(f);
        };

        evbuffer_add_printf(reply_buf, "<br><img src=iface.png?name=%s></p>\n", t->dev);
    };
    pthread_rwlock_unlock(&iface_stat_list_rwlock);
    evbuffer_add_printf(reply_buf, "</body>\n");
    evbuffer_add_printf(reply_buf, "</html>\n");

    return HTTP_OK;
} // www_index_handler()

// WWW "/iface.png" handler
int www_iface_bandwidth_handler(struct evhttp_request *req, struct evbuffer *reply_buf) {
    png_uint_32 width = opt_graph_seconds + 80, height = 100 + 35;
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep img = NULL;
    const char *uri = NULL;
    struct evkeyvalq *req_vars = NULL;
    struct iface_stat *iface = NULL;
    char *req_iface_name = NULL;
    struct iface_stat lif = {0};
    uint32_t *rx_b = NULL, *tx_b = NULL;
    int ret = HTTP_INTERNAL;
    size_t s;

    if (!req || !reply_buf) return HTTP_INTERNAL;

    s = opt_graph_seconds * sizeof(uint32_t);
    rx_b = malloc(s);
    tx_b = malloc(s);
    if (!rx_b || !tx_b) goto error;

    // Parse query
    uri = evhttp_request_get_uri(req);
    if (uri) {
        req_vars = calloc(1, sizeof(struct evkeyvalq));
        if (!req_vars) goto error;
        if (0 != evhttp_parse_query(uri, req_vars)) goto error;

        for (struct evkeyval *var = req_vars->tqh_first; var; var = var->next.tqe_next) {
            if (!strcmp("name", var->key)) {
                req_iface_name = var->value;
            }
        }
    }

    if (!req_iface_name) {
        ret = HTTP_BADREQUEST; // HTTP 400
        goto error;
    }

    // Find iface name
    pthread_rwlock_rdlock(&iface_stat_list_rwlock);
    iface = iface_stat_list;
    while (iface) {
        if (!strcmp(iface->dev, req_iface_name)) {
            //make local clone of iface data
            pthread_mutex_lock(&iface->data_mutex);
                lif = *iface;
                memcpy(rx_b, iface->rx_b, s);
                memcpy(tx_b, iface->tx_b, s);
                lif.rx_b = rx_b;
                lif.tx_b = tx_b;
            pthread_mutex_unlock(&iface->data_mutex);
            break;
        }
        iface = iface->next;
    }
    pthread_rwlock_unlock(&iface_stat_list_rwlock);

    if (!iface) {
        ret = HTTP_NOTFOUND; // HTTP 404
        goto error;
    }

    // Prepare image buffer
    img = malloc(width * height);
    if (!img) goto error;

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) goto error;

    info = png_create_info_struct(png);
    if (!info) goto error;

    // Return point for libpng exceptions
    if (setjmp(png_jmpbuf(png))) goto error;

    png_set_write_fn(png, reply_buf, png_write_to_evbuffer, png_flush_nothing);

    // Prepare PNG image header
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_PALETTE,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_set_PLTE(png, info, palette, sizeof(palette) / sizeof(palette[0]));
    png_set_tRNS(png, info, alpha, sizeof(alpha) / sizeof(alpha[0]), NULL);
    png_write_info(png, info);

    // Generate image content
    generate_iface_graph(img, width, height, &lif);

    // Write PNG image
    for (int y = 0; y < height; y++) {
         png_write_row(png, img + y * width);
    }
    png_write_end(png, NULL);

    ret = HTTP_OK;

error:
    if (req_vars) {
        evhttp_clear_headers(req_vars);
        free(req_vars);
    }
    if (rx_b) free(rx_b);
    if (tx_b) free(tx_b);
    if (png) png_destroy_write_struct(&png, &info);
    if (img) free(img);
    return ret;
} // www_iface_bandwidth_handler()

void fill_description(struct iface_stat *i) {
    FILE *fd = NULL;
    char buffer[1024];
    char *pb;
    size_t s;

    if (!i || !opt_description_file) return;

    fd = fopen(opt_description_file, "rt");
    if (!fd) return;

    while (!feof(fd) && (pb = fgets(buffer,sizeof(buffer),fd))) {
        // trim whitespace at beginning of line
        while (*pb && isblank(*pb)) pb++;
        // skip comments line
        if ((*pb == '\0') || (*pb == '#') || (*pb == ';')) continue;
        // compare with iface name
        if (strncmp(pb, i->dev, strlen(i->dev)) != 0) continue;
        pb += strlen(i->dev);
        while (*pb && isblank(*pb)) pb++;
        if ((*pb == '\0') || (*pb != '=')) continue; // name not equal with iface name
        pb++; // skip '=' char
        // trim white spaces
        while (*pb && isblank(*pb)) pb++;
        s = strlen(pb);
        while (s > 1 && isspace(*(pb + s - 1))) { *(pb + s - 1) = '\0'; s--; }
        // save description
        if (i->description) free(i->description);
        i->description = strdup(pb);
        break;
    }
    fclose(fd);

    return;
} // fill_description()

// Add statistics to iface history
void rx_tx_add(char *dev, uint64_t rx_bytes, uint64_t tx_bytes)
{
    if (!dev) return;

    uint64_t rxbd, txbd;
    // uint64_t rxpd, txpd;
    time_t now = time(NULL);
    struct iface_stat *t, **add;

    // Find iface by name
    pthread_rwlock_rdlock(&iface_stat_list_rwlock);
    t = iface_stat_list;
    while (t) {
        if (!strcmp(t->dev, dev)) break;
        t = t->next;
    }
    pthread_rwlock_unlock(&iface_stat_list_rwlock);

    // If not found (create new)
    if (!t) {
        t = calloc(1, sizeof(struct iface_stat));
        if (!t) return;
        snprintf(t->dev, sizeof(t->dev), "%s", dev);
        pthread_mutex_init(&t->data_mutex, NULL);
        fill_description(t);
        t->cursor_time = now;
        t->rx_b = calloc(opt_graph_seconds, sizeof(uint32_t));
        t->tx_b = calloc(opt_graph_seconds, sizeof(uint32_t));
        if (!t->rx_b || !t->tx_b) {
            if (t->rx_b) free(t->rx_b);
            if (t->tx_b) free(t->tx_b);
            free(t);
            return;
        }
        t->rx_bytes = rx_bytes;
        t->tx_bytes = tx_bytes;

        // insert in list (ordered by iface name)
        pthread_rwlock_wrlock(&iface_stat_list_rwlock);
        add = &iface_stat_list;
        while (*add) {
            if (strcmp(dev, (*add)->dev) < 0) break;
            add = &(*add)->next;
        }
        t->next = *add;
        (*add) = t;
        pthread_rwlock_unlock(&iface_stat_list_rwlock);
        return;
    }

    pthread_mutex_lock(&t->data_mutex);
         t->mark_for_delete = 0;

        // Roll cursor to current time
        while (t->cursor_time < now) {
            t->cursor++;
            t->cursor %= opt_graph_seconds;
            t->cursor_time++;
            t->rx_b[t->cursor] = 0;
            t->tx_b[t->cursor] = 0;
        }

        if (rx_bytes >= t->rx_bytes) {
            rxbd = rx_bytes - t->rx_bytes;
        } else {
            // 64-bit counter overflow
            if (t->overflow_64bit > 0 && rx_bytes >= t->overflow_64bit) {
                rxbd = ~(uint64_t)0 - t->rx_bytes + rx_bytes; // real overflow from last values
            } else {
                fill_description(t);
                rxbd = rx_bytes; // flapping interface
            }
        }
        t->rx_bytes = rx_bytes;

        if (tx_bytes >= t->tx_bytes) {
            txbd = tx_bytes - t->tx_bytes;
        } else {
            if (t->overflow_64bit > 0 && tx_bytes >= t->overflow_64bit) {
                txbd = ~(uint64_t)0 - t->tx_bytes + tx_bytes;
            } else {
                fill_description(t);
                txbd = tx_bytes;
            }
        }
        t->tx_bytes = tx_bytes;

        t->rx_b[t->cursor] += rxbd;
        t->tx_b[t->cursor] += txbd;
    pthread_mutex_unlock(&t->data_mutex);
} // rx_tx_add()

int read_proc_net_dev() {
    // kernel >= 3.8.9 use 64-bit counters for interfaces statistics
    // struct rtnl_link_stats64 s;
    // defined in linux/if_link.h and generated
    // in kernel/net/core/net-procfs.c : dev_seq_printf_stats()
    uint64_t s_rx_bytes, s_rx_packets, s_rx_errors, s_rx_dropped
        , s_rx_fifo_errors, s_rx_length_errors, s_rx_compressed, s_multicast
        , s_tx_bytes, s_tx_packets, s_tx_errors, s_tx_dropped
        , s_tx_fifo_errors, s_collisions, s_tx_carrier_errors, s_tx_compressed;

    char dev_name[IF_NAMESIZE + 1];
    char buffer[2048];
    char *pb, *pc;
    int ret;
    FILE *procdev;
    int skip_space = 0;

    procdev = fopen("/proc/net/dev","rt");
    if (!procdev) exit(10);

    // skip file header (two lines)
    pb = fgets(buffer, sizeof(buffer), procdev);
    pb = fgets(buffer, sizeof(buffer), procdev);

    while (!feof(procdev) && (pb = fgets(buffer, sizeof(buffer), procdev))) {
        // trim whitespace at beginning of line
        pb = buffer; while (*pb && *pb == ' ') pb++;

        // save device name
        pc = dev_name; ret = IF_NAMESIZE;
        while (*pb != ':' && ret > 0) { *pc = *pb; pb++; pc++; ret--; };
        *pc=0; pb++;

        // trim whitespaces between numbers
        pc = buffer; ret = sizeof(buffer);
        while (*pb == ' ' && ret > 0) { pb++; ret--; }
        while (*pb != '\n' && ret > 0) {
            *pc = *pb;
            if (*pb == ' ') {
                if (!skip_space) { pc++; skip_space = 1; }
            } else {
                pc++;
                skip_space = 0;
            };
            pb++; ret--;
        }; *pc = 0;

        // scan numbers as in kernel /net/core/net-procfs.c : dev_seq_printf_stats()
        if (16 == sscanf(buffer,"%" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
            , &s_rx_bytes, &s_rx_packets, &s_rx_errors, &s_rx_dropped
            , &s_rx_fifo_errors, &s_rx_length_errors, &s_rx_compressed, &s_multicast
            , &s_tx_bytes, &s_tx_packets, &s_tx_errors, &s_tx_dropped
            , &s_tx_fifo_errors, &s_collisions, &s_tx_carrier_errors, &s_tx_compressed
            )
        ) {
            rx_tx_add(dev_name, s_rx_bytes, s_tx_bytes);
        }
    } // while feof

    fclose(procdev);
    return 0;
} // read_proc_net_dev()

// Thread to update iface statistics 5 times every second
void  *update_statistics(void *args) {
    (void) args;
    struct iface_stat *t;

    while (2 != program_state) {
        // make all iface marked_for_delete
        pthread_rwlock_rdlock(&iface_stat_list_rwlock);
        t = iface_stat_list;
        while (t) {
            pthread_mutex_lock(&t->data_mutex);
                t->mark_for_delete = 1;
            pthread_mutex_unlock(&t->data_mutex);
            t = t->next;
        }
        pthread_rwlock_unlock(&iface_stat_list_rwlock);

        read_proc_net_dev();

        usleep(1000000 / 5);
    }
    pthread_exit(NULL);
} // update_statistics()

// Array of URL's served by HTTP
typedef int (*function_url_handler_t)(struct evhttp_request *, struct evbuffer *);

struct http_uri {
    char *uri;
    char *content_type; // if NULL - "text/html;charset=utf-8"
    function_url_handler_t handler;
} http_uri_list[] = {
    { "/", NULL, &www_index_handler}
    ,{ "/index.html", NULL, &www_index_handler}
    ,{ "/iface.png", "image/png", &www_iface_bandwidth_handler}
    ,{ NULL, NULL, NULL} // end of list
}; // http_uri_list

// Common HTTP handler
void http_process_request(struct evhttp_request *req, void *arg) {
    struct evhttp_uri *uri_parsed = NULL;
    const char *conttype = "text/html;charset=utf-8";
    int http_code = HTTP_NOTFOUND;
    const char *http_message = "Not Found";

    uri_parsed = evhttp_uri_parse(req->uri);
    if (!uri_parsed) {
        evhttp_send_error(req, HTTP_BADREQUEST, 0);
        return;
    }

    struct evbuffer *buf = evbuffer_new();
    if (!buf) {
        evhttp_send_error(req, HTTP_INTERNAL, "Can't allocate memory for reply");
        if (uri_parsed) evhttp_uri_free(uri_parsed);
        return;
    }

    char *path = evhttp_decode_uri(evhttp_uri_get_path(uri_parsed));
    if (path) {
        struct http_uri *u = http_uri_list;
        while (u->uri) {
            if (0 == strcmp(path, u->uri)) {
                if (u->content_type) conttype = u->content_type;
                http_code = u->handler(req, buf);
                break;
            }
            u++;
        }
        free(path);
    } else http_code = HTTP_INTERNAL;

    switch (http_code) {
    case HTTP_OK:
        evhttp_add_header(req->output_headers, "Expires", "Mon, 01 Jan 1995 00:00:00 GMT");
        evhttp_add_header(req->output_headers, "Cache-Control", "no-cache, must-revalidate");
        evhttp_add_header(req->output_headers, "Pragma", "no-cache");
        if (strlen(conttype) > 0) {
            evhttp_add_header(req->output_headers, "Content-type", conttype);
        };
        http_message = "OK";
        break;
    //case HTTP_UNAUTHORIZED: http_message = "Unauthorized"; break;
    case HTTP_BADREQUEST: http_message = "Wrong request"; break;
    case HTTP_NOTFOUND: http_message = "Not Found"; break;
    case HTTP_MOVEPERM: http_message = "Moved Permanently"; break;
    case HTTP_BADMETHOD: http_message = "Bad method"; break;
    default:
        http_code = HTTP_INTERNAL;
        http_message = "Internal server error";
    } // switch
    evhttp_send_reply(req, http_code, http_message, buf);

    if (buf) evbuffer_free(buf);
    if (uri_parsed) evhttp_uri_free(uri_parsed);
} // http_process_request()

void signal_handler(int sig) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            program_state = 2; //exit main cycle
            event_base_loopbreak(base);
            break;
    }
} // signal_handler()

void print_help(const char *prog) {
    printf("%s network interface bandwidth graphs as HTTP server\n", prog);
    printf("Usage: %s [options]\n", prog);
    printf("\t-h           - this help\n");
    printf("\t-d [file]    - interface description file (line format \"<ifname> = <description>\")\n");
    printf("\t               Use full path to file, daemonized process change self CWD to \"/\".\n");
    printf("\t-l [address] - listening address (default %s)\n", opt_listening_address);
    printf("\t-p [port]    - listening port (default %d)\n", opt_server_port);
    printf("\t-s [seconds] - history length in seconds (default %ld)\n", opt_graph_seconds);
    exit(0);
} // print_help()

int main(int argc, char **argv) {
    int opt = 0;

    while ( (opt = getopt(argc, argv, "hd:l:p:s:")) != -1)
    switch (opt) {
        case 'h': print_help(argv[0]); break;
        case 'd': opt_description_file = optarg; break;
        case 'l': opt_listening_address = optarg; break;
        case 'p': opt_server_port = atoi(optarg); break;
        case 's': opt_graph_seconds = (size_t)atoi(optarg); break;
        case '?':
            fprintf(stderr,"Unknown option: %c\n", optopt);
            return 1;
            break;
    }

    // Drop root privileges to nobody
    if (getuid() == 0) setuid(65534);

    if (opt_graph_seconds + 80 > 1920) {
        fprintf(stderr, "Graph width can't be more than 1920 pixels in width (%ld + 80)\n", opt_graph_seconds);
        return 1;
    }

    evthread_use_pthreads();

    base = event_base_new();
    if (!base) {
        fprintf(stderr, "Can't create event_base\n");
        return 1;
    }

    http = evhttp_new(base);
    if (!http) {
        fprintf(stderr, "Can't creat evhttp\n");
        return 1;
    }

    evhttp_set_gencb(http, http_process_request, NULL);

    handle = evhttp_bind_socket_with_handle(http, opt_listening_address, opt_server_port);
    if (!handle) {
        fprintf(stderr, "Can't bind to port %d\n", opt_server_port);
        return 1;
    }

    if (daemon(0, 0) != 0) {
        fprintf(stderr,"Can't daemonize process!\n");
        return 1;
    }

    read_proc_net_dev();
    if (0 != pthread_create(&thread_id, NULL, update_statistics, NULL)) {
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    event_base_dispatch(base);

    pthread_join(thread_id, NULL);
    evhttp_free(http);
    event_base_free(base);

    return 0;
}
