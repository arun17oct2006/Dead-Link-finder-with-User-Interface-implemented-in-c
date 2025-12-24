#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <regex.h>
#include <stdbool.h>

#define MAX_DEPTH 3
#define MAX_LINKS 1000

typedef struct {
    GtkEntry *entry;
    GtkTextBuffer *buffer;
    GtkSpinner *spinner;
    GtkButton *button;
    GtkButton *stop_button;
    GtkButton *export_button;
    GtkStack *stack;
    volatile bool stop_flag;
} AppWidgets;

typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

/* ---------- libcurl helpers ---------- */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

static char *fetch_remote(const char *url) {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk = {malloc(1), 0};
    curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DeadLinkFinder/1.0");
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        free(chunk.memory);
        chunk.memory = NULL;
    }
    curl_easy_cleanup(curl);
    return chunk.memory;
}

static int check_link(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    long code = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DeadLinkFinder/1.0");
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    return (int)code;
}

static char *normalize_url(const char *base, const char *href) {
    if (strstr(href, "http://") || strstr(href, "https://"))
        return strdup(href);
    if (href[0] == '/')
        return g_strdup_printf("%s%s", base, href);
    return NULL;
}

static void extract_links(const char *html, const char *base, char **links, int *count) {
    regex_t regex;
    regcomp(&regex, "<a[^>]+href=[\"']([^\"']+)[\"']", REG_ICASE | REG_EXTENDED);
    regmatch_t matches[2];
    const char *cursor = html;
    while (*count < MAX_LINKS && regexec(&regex, cursor, 2, matches, 0) == 0) {
        int len = matches[1].rm_eo - matches[1].rm_so;
        char href[1024];
        strncpy(href, cursor + matches[1].rm_so, len);
        href[len] = '\0';
        char *full = normalize_url(base, href);
        if (full) links[(*count)++] = full;
        cursor += matches[0].rm_eo;
    }
    regfree(&regex);
}

static gboolean is_same_domain(const char *base, const char *url) {
    return strncmp(base, url, strlen(base)) == 0;
}

/* ---------- Safe UI updates from threads ---------- */
typedef struct {
    GtkTextBuffer *buffer;
    char *msg;
} MessageData;

static gboolean append_message_idle(gpointer user_data) {
    MessageData *m = user_data;
    gtk_text_buffer_insert_at_cursor(m->buffer, m->msg, -1);
    free(m->msg);
    free(m);
    return G_SOURCE_REMOVE;
}

static void append_message(GtkTextBuffer *buffer, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *msg = g_strdup_vprintf(fmt, args);
    va_end(args);

    MessageData *m = g_malloc(sizeof(MessageData));
    m->buffer = buffer;
    m->msg = msg;
    g_idle_add(append_message_idle, m);
}

/* ---------- Recursive crawler ---------- */
static void dfs(const char *url, const char *base, int depth, GHashTable *visited,
                GtkTextBuffer *buffer, volatile bool *stop_flag) {
    if (*stop_flag || depth > MAX_DEPTH || g_hash_table_contains(visited, url))
        return;
    g_hash_table_add(visited, g_strdup(url));

    char *html = fetch_remote(url);
    if (!html) {
        append_message(buffer, "Unreachable or missing: %s\n", url);
        return;
    }

    char *links[MAX_LINKS];
    int count = 0;
    extract_links(html, base, links, &count);
    free(html);

    for (int i = 0; i < count; i++) {
        if (*stop_flag) {
            for (int j = i; j < count; j++)
                free(links[j]);
            break;
        }
       
        if (!is_same_domain(base, links[i])) {
            free(links[i]);
            continue;
        }
        int code = check_link(links[i]);
        if (code >= 400) {
            append_message(buffer, "Dead link (%d): %s\n", code, links[i]);
        }
        dfs(links[i], base, depth + 1, visited, buffer, stop_flag);
        free(links[i]);
    }
}

/* ---------- Worker thread ---------- */
typedef struct {
    char *input;
    GtkTextBuffer *buffer;
    GtkSpinner *spinner;
    GtkButton *button;
    GtkButton *stop_button;
    GtkButton *export_button;
    volatile bool *stop_flag;
} WorkerArgs;

typedef struct {
    GtkSpinner *spinner;
    GtkButton *button;
    GtkButton *stop_button;
    GtkButton *export_button;
} CleanupData;

static gboolean cleanup_ui_idle(gpointer user_data) {
    CleanupData *c = user_data;
    gtk_spinner_stop(c->spinner);
    gtk_widget_set_sensitive(GTK_WIDGET(c->button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(c->stop_button), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(c->export_button), TRUE);
    free(c);
    return G_SOURCE_REMOVE;
}

static gpointer worker_thread(gpointer arg) {
    WorkerArgs *w = arg;
    GHashTable *visited = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    dfs(w->input, w->input, 0, visited, w->buffer, w->stop_flag);
    g_hash_table_destroy(visited);

    if (*w->stop_flag) {
        append_message(w->buffer, "\n--- Scan stopped by user ---\n");
    } else {
        append_message(w->buffer, "\n--- Scan complete ---\n");
    }

    CleanupData *cleanup = malloc(sizeof(CleanupData));
    cleanup->spinner = w->spinner;
    cleanup->button = w->button;
    cleanup->stop_button = w->stop_button;
    cleanup->export_button = w->export_button;
    g_idle_add(cleanup_ui_idle, cleanup);

    free(w->input);
    free(w);
    return NULL;
}

/* ---------- Button callbacks ---------- */
static void on_get_started_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *w = user_data;
    gtk_stack_set_visible_child_name(w->stack, "main");
}

static void on_button_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *w = user_data;
    const char *input = gtk_editable_get_text(GTK_EDITABLE(w->entry));
    gtk_text_buffer_set_text(w->buffer, "Scanning for dead links...\n", -1);
    gtk_spinner_start(w->spinner);
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(w->stop_button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(w->export_button), FALSE);
   
    w->stop_flag = false;

    WorkerArgs *args = malloc(sizeof(WorkerArgs));
    args->input = strdup(input);
    args->buffer = w->buffer;
    args->spinner = w->spinner;
    args->button = w->button;
    args->stop_button = w->stop_button;
    args->export_button = w->export_button;
    args->stop_flag = &w->stop_flag;

    g_thread_new("crawler", worker_thread, args);
}

static void on_stop_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *w = user_data;
    w->stop_flag = true;
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
}

static void on_file_dialog_response(GtkWidget *dialog, int response, gpointer user_data) {
    AppWidgets *w = user_data;
   
    if (response == GTK_RESPONSE_ACCEPT) {
        GtkTextBuffer *buffer = w->buffer;
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        char *path = g_file_get_path(file);
       
        GtkTextIter start, end;
        gtk_text_buffer_get_start_iter(buffer, &start);
        gtk_text_buffer_get_end_iter(buffer, &end);
        char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
       
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%s", text);
            fclose(f);
           
            GtkWidget *msg_dialog = gtk_message_dialog_new(
                NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                "Dead links exported successfully to:\n%s", path);
            g_signal_connect(msg_dialog, "response", G_CALLBACK(gtk_window_destroy), NULL);
            gtk_widget_show(msg_dialog);
        }
       
        g_free(text);
        g_free(path);
        g_object_unref(file);
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void on_export_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *w = user_data;
   
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Save Dead Links",
        NULL,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT,
        NULL);
   
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "deadlinks.txt");
    g_signal_connect(dialog, "response", G_CALLBACK(on_file_dialog_response), w);
    gtk_widget_show(dialog);
}

/* ---------- Main ---------- */
int main(int argc, char *argv[]) {
    gtk_init();

    GtkWidget *window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "Dead Link Finder");
    gtk_window_set_default_size(GTK_WINDOW(window), 700, 500);

    /* Stack to switch between welcome and main screens */
    GtkWidget *stack = gtk_stack_new();
    gtk_window_set_child(GTK_WINDOW(window), stack);

    /* ========== WELCOME SCREEN ========== */
    GtkWidget *welcome_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_valign(welcome_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(welcome_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(welcome_box, 40);
    gtk_widget_set_margin_bottom(welcome_box, 40);
    gtk_widget_set_margin_start(welcome_box, 40);
    gtk_widget_set_margin_end(welcome_box, 40);

    /* Icon/Image */
    GtkWidget *image = gtk_image_new_from_file("/home/arun/Downloads/brba1.jpg");
    gtk_image_set_pixel_size(GTK_IMAGE(image),500);
    gtk_box_append(GTK_BOX(welcome_box), image);

    /* Title */
    GtkWidget *title_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title_label),
        "<span size='xx-large' weight='bold'>Dead Link Finder</span>");
    gtk_box_append(GTK_BOX(welcome_box), title_label);

    /* Description */
    GtkWidget *desc_label = gtk_label_new(
        "Scan your website for broken links and dead URLs.\n"
        "Find and fix 404 errors quickly and easily.");
    gtk_label_set_justify(GTK_LABEL(desc_label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_opacity(desc_label, 0.7);
    gtk_box_append(GTK_BOX(welcome_box), desc_label);

    /* Get Started Button */
    GtkWidget *get_started_btn = gtk_button_new_with_label("Get Started");
    gtk_widget_set_size_request(get_started_btn, 200, 50);
    gtk_widget_set_halign(get_started_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(get_started_btn, "suggested-action");
    gtk_box_append(GTK_BOX(welcome_box), get_started_btn);

    gtk_stack_add_named(GTK_STACK(stack), welcome_box, "welcome");

    /* ========== MAIN SCREEN ========== */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    /* Top image - centered */
    GtkWidget *top_image = gtk_image_new_from_file("/home/arun/Downloads/t.jpg");
    gtk_image_set_pixel_size(GTK_IMAGE(top_image),250);
    gtk_widget_set_halign(top_image, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(top_image, 10);
    gtk_widget_set_margin_bottom(top_image, 20);
    gtk_box_append(GTK_BOX(box), top_image);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Enter website URL (e.g., https://example.com)");
    gtk_box_append(GTK_BOX(box), entry);

    /* Button container */
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(box), button_box);

    GtkWidget *button = gtk_button_new_with_label("Find Dead Links");
    gtk_widget_set_hexpand(button, TRUE);
    gtk_box_append(GTK_BOX(button_box), button);

    GtkWidget *stop_button = gtk_button_new_with_label("Stop");
    gtk_widget_set_sensitive(stop_button, FALSE);
    gtk_box_append(GTK_BOX(button_box), stop_button);

    GtkWidget *export_button = gtk_button_new_with_label("Export to File");
    gtk_box_append(GTK_BOX(button_box), export_button);

    GtkWidget *spinner = gtk_spinner_new();
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), spinner);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(box), scrolled);

    GtkWidget *textview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), textview);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));

    gtk_stack_add_named(GTK_STACK(stack), box, "main");

    /* Set welcome as initial screen */
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "welcome");

    AppWidgets *widgets = g_malloc(sizeof(AppWidgets));
    widgets->entry = GTK_ENTRY(entry);
    widgets->buffer = buffer;
    widgets->spinner = GTK_SPINNER(spinner);
    widgets->button = GTK_BUTTON(button);
    widgets->stop_button = GTK_BUTTON(stop_button);
    widgets->export_button = GTK_BUTTON(export_button);
    widgets->stack = GTK_STACK(stack);
    widgets->stop_flag = false;

    g_signal_connect(get_started_btn, "clicked", G_CALLBACK(on_get_started_clicked), widgets);
    g_signal_connect(button, "clicked", G_CALLBACK(on_button_clicked), widgets);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_stop_clicked), widgets);
    g_signal_connect(export_button, "clicked", G_CALLBACK(on_export_clicked), widgets);

    gtk_window_present(GTK_WINDOW(window));

    /* GTK4 main loop */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_signal_connect(window, "close-request", G_CALLBACK(g_main_loop_quit), loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    return 0;
}

