#include <gtk/gtk.h>
#include <gst/gst.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <math.h>

#define AUDIO_FILE "./bgm.mp3"

static GstElement *audio_pipeline = NULL;
static gdouble wave_phase = 0.0;     
static gdouble target_volume = 0.5;  
static gdouble current_volume = 0.5; 
static guint fade_timer_id = 0;      

static GtkWidget *game_list_box = NULL; 
static char *current_game_dir = NULL;   

// Returns the full path to Ruffle's save directory (~/.local/share/ruffle/SharedObjects)
static char* get_ruffle_saves_dir(void) {
    const char *home = g_get_home_dir();
    return g_build_filename(home, ".local", "share", "ruffle", "SharedObjects", NULL);
}

// Helper to set pipeline volume directly
static void set_pipeline_volume(gdouble vol) {
    if (audio_pipeline) {
        g_object_set(G_OBJECT(audio_pipeline), "volume", vol, NULL);
    }
}

static gboolean on_fade_step(gpointer user_data) {
    gboolean is_fading_in = GPOINTER_TO_INT(user_data);

    if (is_fading_in) {
        current_volume += 0.025;
        if (current_volume >= target_volume) {
            current_volume = target_volume;
            set_pipeline_volume(current_volume);
            fade_timer_id = 0;
            return G_SOURCE_REMOVE;
        }
    } else {
        current_volume -= 0.025;
        if (current_volume <= 0.0) {
            current_volume = 0.0;
            set_pipeline_volume(current_volume);
            gst_element_set_state(audio_pipeline, GST_STATE_PAUSED);
            fade_timer_id = 0;
            return G_SOURCE_REMOVE;
        }
    }

    set_pipeline_volume(current_volume);
    return G_SOURCE_CONTINUE;
}

static void fade_out_bgm(void) {
    if (!audio_pipeline) return;
    if (fade_timer_id != 0) g_source_remove(fade_timer_id);
    fade_timer_id = g_timeout_add(20, on_fade_step, GINT_TO_POINTER(FALSE));
}

static void fade_in_bgm(void) {
    if (!audio_pipeline) return;
    if (fade_timer_id != 0) g_source_remove(fade_timer_id);
    gst_element_set_state(audio_pipeline, GST_STATE_PLAYING);
    fade_timer_id = g_timeout_add(20, on_fade_step, GINT_TO_POINTER(TRUE));
}

static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        gst_element_seek_simple(audio_pipeline, GST_FORMAT_TIME,
                               GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
        gst_element_set_state(audio_pipeline, GST_STATE_PLAYING);
    }
    return TRUE;
}

static void on_volume_changed(GtkRange *range, gpointer user_data) {
    target_volume = gtk_range_get_value(range);
    if (fade_timer_id == 0 && current_volume > 0.0) {
        current_volume = target_volume;
        set_pipeline_volume(current_volume);
    }
}

static void on_game_finished(GPid pid, gint status, gpointer user_data) {
    g_spawn_close_pid(pid);
    fade_in_bgm();
}

static void on_game_clicked(GtkWidget *widget, gpointer data) {
    const char *swf_path = (const char *)data;
    fade_out_bgm();

    pid_t pid = fork();
    if (pid == 0) {
        char *args[] = {"ruffle", (char *)swf_path, NULL};
        execvp("ruffle", args);
        g_printerr("ERROR: Failed to run 'ruffle'.\n");
        _exit(1);
    } else if (pid > 0) {
        g_child_watch_add(pid, on_game_finished, NULL);
    }
}

// Deletes ONLY the save files associated with the clicked game
static void on_delete_single_game_save(GtkWidget *widget, gpointer data) {
    const char *game_name = (const char *)data;

    // Create confirmation dialog
    char *msg = g_strdup_printf("Delete save data for '%s'?", game_name);
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO,
        "%s", msg
    );
    g_free(msg);

    gint result = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (result == GTK_RESPONSE_YES) {
        char *saves_dir = get_ruffle_saves_dir();
        
        // Strip ".swf" extension to match save naming patterns
        char clean_name[256];
        strncpy(clean_name, game_name, sizeof(clean_name) - 1);
        clean_name[sizeof(clean_name) - 1] = '\0';
        char *ext = strrchr(clean_name, '.');
        if (ext) *ext = '\0';

        // Delete matches in Ruffle's SharedObjects directory
        char *cmd = g_strdup_printf("find \"%s\" -iname \"*%s*\" -delete 2>/dev/null", saves_dir, clean_name);
        system(cmd);

        g_free(cmd);
        g_free(saves_dir);
    }
}

// Clears and populates the game list, adding a Trash Bin button next to each game
static void load_games_from_dir(const char *dir_path) {
    if (!game_list_box || !dir_path) return;

    GList *children = gtk_container_get_children(GTK_CONTAINER(game_list_box));
    for (GList *iter = children; iter != NULL; iter = g_list_next(iter)) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);

    DIR *d = opendir(dir_path);
    if (d) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            if (strstr(dir->d_name, ".swf")) {
                char *full_path = g_strdup_printf("%s/%s", dir_path, dir->d_name);
                char *game_name = g_strdup(dir->d_name);

                // Row HBox to hold [Game Title Button] + [Bin Save Button]
                GtkWidget *row_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

                // Game Title Button
                GtkWidget *btn = gtk_button_new_with_label(game_name);
                GtkStyleContext *btn_ctx = gtk_widget_get_style_context(btn);
                gtk_style_context_add_class(btn_ctx, "aero-card");
                gtk_widget_set_hexpand(btn, TRUE);
                g_signal_connect(btn, "clicked", G_CALLBACK(on_game_clicked), full_path);

                // Individual Bin Button
                GtkWidget *bin_btn = gtk_button_new_with_label("🗑️");
                GtkStyleContext *bin_ctx = gtk_widget_get_style_context(bin_btn);
                gtk_style_context_add_class(bin_ctx, "bin-btn");
                gtk_widget_set_tooltip_text(bin_btn, "Delete save file for this game");
                g_signal_connect(bin_btn, "clicked", G_CALLBACK(on_delete_single_game_save), game_name);

                gtk_box_pack_start(GTK_BOX(row_hbox), btn, TRUE, TRUE, 0);
                gtk_box_pack_end(GTK_BOX(row_hbox), bin_btn, FALSE, FALSE, 0);

                gtk_list_box_insert(GTK_LIST_BOX(game_list_box), row_hbox, -1);
            }
        }
        closedir(d);
        gtk_widget_show_all(game_list_box);
    }
}

static void on_choose_folder_clicked(GtkWidget *widget, gpointer window) {
    GtkFileChooserNative *native = gtk_file_chooser_native_new(
        "Select Flash Games Directory",
        GTK_WINDOW(window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Select Folder",
        "_Cancel"
    );

    if (current_game_dir) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(native), current_game_dir);
    }

    gint res = gtk_native_dialog_run(GTK_NATIVE_DIALOG(native));
    if (res == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(native);
        g_free(current_game_dir);
        current_game_dir = gtk_file_chooser_get_filename(chooser);
        load_games_from_dir(current_game_dir);
    }

    g_object_unref(native);
}

static void on_open_saves_clicked(GtkWidget *widget, gpointer window) {
    char *saves_dir = get_ruffle_saves_dir();
    g_mkdir_with_parents(saves_dir, 0755);

    char *cmd = g_strdup_printf("xdg-open \"%s\"", saves_dir);
    system(cmd);

    g_free(cmd);
    g_free(saves_dir);
}

static gboolean on_wave_tick(GtkWidget *drawing_area) {
    wave_phase += 0.01;
    if (wave_phase > M_PI * 2) wave_phase -= M_PI * 2;
    gtk_widget_queue_draw(drawing_area);
    return G_SOURCE_CONTINUE;
}

static gboolean on_draw_wave(GtkWidget *widget, cairo_t *cr, gpointer data) {
    guint width = gtk_widget_get_allocated_width(widget);
    guint height = gtk_widget_get_allocated_height(widget);

    cairo_set_source_rgba(cr, 0.12, 0.14, 0.18, 0.98);
    cairo_paint(cr);

    int num_waves = 3;
    for (int w = 0; w < num_waves; w++) {
        cairo_new_path(cr);

        double wave_offset = wave_phase + (w * 1.5);
        double amplitude = 30.0 + (w * 12.0);
        double frequency = 0.005 - (w * 0.001);
        double base_y = (height / 2.0) + (w * 20.0) - 20.0;

        cairo_move_to(cr, 0, height);
        cairo_line_to(cr, 0, base_y);

        for (int x = 0; x <= width; x += 5) {
            double y = base_y + sin(x * frequency + wave_offset) * amplitude;
            cairo_line_to(cr, x, y);
        }

        cairo_line_to(cr, width, height);
        cairo_close_path(cr);

        if (w == 0) cairo_set_source_rgba(cr, 0.25, 0.28, 0.35, 0.20);
        else if (w == 1) cairo_set_source_rgba(cr, 0.35, 0.38, 0.45, 0.15);
        else cairo_set_source_rgba(cr, 0.18, 0.20, 0.26, 0.25);

        cairo_fill(cr);
    }

    return FALSE;
}

static void init_audio(void) {
    gst_init(NULL, NULL);

    char abs_path[PATH_MAX];
    if (realpath(AUDIO_FILE, abs_path) != NULL) {
        char uri[PATH_MAX + 8];
        snprintf(uri, sizeof(uri), "file://%s", abs_path);

        audio_pipeline = gst_element_factory_make("playbin", "bgm-player");
        g_object_set(G_OBJECT(audio_pipeline), "uri", uri, NULL);
        
        set_pipeline_volume(current_volume);

        GstBus *bus = gst_element_get_bus(audio_pipeline);
        gst_bus_add_watch(bus, on_bus_message, NULL);
        gst_object_unref(bus);
        
        gst_element_set_state(audio_pipeline, GST_STATE_PLAYING);
    } else {
        g_printerr("Could not find background audio file: %s\n", AUDIO_FILE);
    }
}

static void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    GdkDisplay *display = gdk_display_get_default();
    GdkScreen *screen = gdk_display_get_default_screen(display);

    const char *css = 
        "list { background: transparent; }"
        "row { background: transparent; padding: 4px 10px; }"
        "row:hover { background: transparent; }"
        ".aero-card {"
        "  background: none; border: none; box-shadow: none;"
        "  color: #ffffff; font-weight: bold; font-size: 16px;"
        "  text-shadow: 0 2px 5px rgba(0, 0, 0, 0.95);"
        "  padding: 10px 14px; halign: start; transition: all 0.2s ease;"
        "}"
        ".aero-card:hover {"
        "  background: none; color: #ffffff;"
        "  text-shadow: 0 0 14px rgba(255, 255, 255, 0.95), 0 2px 4px rgba(0,0,0,0.95);"
        "}"
        ".bin-btn {"
        "  background: rgba(255, 255, 255, 0.08);"
        "  border: 1px solid rgba(255, 255, 255, 0.2);"
        "  border-radius: 4px; color: #ffffff;"
        "  padding: 6px 10px; opacity: 0.65; transition: all 0.2s ease;"
        "}"
        ".bin-btn:hover {"
        "  background: rgba(220, 53, 69, 0.4);"
        "  border-color: rgba(220, 53, 69, 0.8);"
        "  opacity: 1.0;"
        "}"
        ".volume-bar {"
        "  padding: 12px 16px; background: rgba(0, 0, 0, 0.35);"
        "}"
        ".volume-label {"
        "  color: #ffffff; font-size: 13px; font-weight: bold;"
        "  text-shadow: 0 1px 3px rgba(0,0,0,0.8); margin-right: 6px;"
        "}"
        ".action-btn {"
        "  background: rgba(255, 255, 255, 0.15);"
        "  border: 1px solid rgba(255, 255, 255, 0.3);"
        "  border-radius: 4px; color: #ffffff; font-weight: bold;"
        "  padding: 4px 10px; margin-right: 6px;"
        "}"
        ".action-btn:hover {"
        "  background: rgba(255, 255, 255, 0.3);"
        "}";

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void activate(GtkApplication *app, gpointer user_data) {
    load_css();
    init_audio();

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Aero Flash Launcher");
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 650);

    GdkScreen *screen = gdk_screen_get_default();
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual) gtk_widget_set_visual(window, visual);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(window), overlay);

    GtkWidget *drawing_area = gtk_drawing_area_new();
    g_signal_connect(G_OBJECT(drawing_area), "draw", G_CALLBACK(on_draw_wave), NULL);
    gtk_container_add(GTK_CONTAINER(overlay), drawing_area);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    game_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(game_list_box), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scrolled_window), game_list_box);

    gtk_box_pack_start(GTK_BOX(main_vbox), scrolled_window, TRUE, TRUE, 0);

    // Bottom Control Bar
    GtkWidget *vol_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkStyleContext *vol_ctx = gtk_widget_get_style_context(vol_hbox);
    gtk_style_context_add_class(vol_ctx, "volume-bar");

    GtkWidget *folder_btn = gtk_button_new_with_label("📁 Games");
    GtkStyleContext *fbtn_ctx = gtk_widget_get_style_context(folder_btn);
    gtk_style_context_add_class(fbtn_ctx, "action-btn");
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_choose_folder_clicked), window);

    GtkWidget *saves_btn = gtk_button_new_with_label("💾 Open Saves Folder");
    GtkStyleContext *sbtn_ctx = gtk_widget_get_style_context(saves_btn);
    gtk_style_context_add_class(sbtn_ctx, "action-btn");
    g_signal_connect(saves_btn, "clicked", G_CALLBACK(on_open_saves_clicked), window);

    GtkWidget *vol_label = gtk_label_new("♫");
    GtkStyleContext *lbl_ctx = gtk_widget_get_style_context(vol_label);
    gtk_style_context_add_class(lbl_ctx, "volume-label");

    GtkWidget *vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.05);
    gtk_scale_set_draw_value(GTK_SCALE(vol_scale), FALSE);
    gtk_range_set_value(GTK_RANGE(vol_scale), target_volume);
    gtk_widget_set_hexpand(vol_scale, TRUE);

    g_signal_connect(vol_scale, "value-changed", G_CALLBACK(on_volume_changed), NULL);

    gtk_box_pack_start(GTK_BOX(vol_hbox), folder_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vol_hbox), saves_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vol_hbox), vol_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vol_hbox), vol_scale, TRUE, TRUE, 0);

    gtk_box_pack_end(GTK_BOX(main_vbox), vol_hbox, FALSE, FALSE, 0);

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), main_vbox);

    current_game_dir = g_strdup("./games");
    load_games_from_dir(current_game_dir);

    g_timeout_add(16, (GSourceFunc)on_wave_tick, drawing_area);

    gtk_widget_show_all(window);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.aero.flashlauncher", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    
    if (audio_pipeline) {
        gst_element_set_state(audio_pipeline, GST_STATE_NULL);
        gst_object_unref(audio_pipeline);
    }
    g_free(current_game_dir);
    g_object_unref(app);
    return status;
}

