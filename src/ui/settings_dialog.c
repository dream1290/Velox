/* vlx_settings_dialog.c — AdwPreferencesDialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pages:
 *   Playback  — default volume, resume playback, speed presets
 *   Video     — HW acceleration toggle, deinterlace, colour profile
 *   Audio     — output device, normalisation, audio offset
 *   Subtitles — font, size, colour, background opacity
 *   Interface — dark mode, playlist behaviour, OSD timeout
 */

#include "ui/settings_dialog.h"

struct _VlxSettingsDialog {
    AdwPreferencesDialog parent_instance;
};

G_DEFINE_TYPE (VlxSettingsDialog, vlx_settings_dialog, ADW_TYPE_PREFERENCES_DIALOG)

static AdwPreferencesPage *
build_playback_page (void)
{
    AdwPreferencesPage  *page  = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
    adw_preferences_page_set_title (page, "Playback");
    adw_preferences_page_set_icon_name (page, "media-playback-start-symbolic");

    AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (grp, "General");

    /* Resume playback */
    AdwSwitchRow *resume = ADW_SWITCH_ROW (adw_switch_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (resume),
                                   "Resume playback");
    adw_action_row_set_subtitle (ADW_ACTION_ROW (resume),
                                 "Pick up where you left off");
    adw_preferences_group_add (grp, GTK_WIDGET (resume));

    /* Default volume */
    AdwSpinRow *vol = ADW_SPIN_ROW (adw_spin_row_new_with_range (0, 100, 5));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (vol), "Default volume");
    adw_preferences_group_add (grp, GTK_WIDGET (vol));

    adw_preferences_page_add (page, grp);
    return page;
}

static AdwPreferencesPage *
build_video_page (void)
{
    AdwPreferencesPage  *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
    adw_preferences_page_set_title    (page, "Video");
    adw_preferences_page_set_icon_name (page, "video-display-symbolic");

    AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (grp, "Hardware");

    AdwSwitchRow *vaapi = ADW_SWITCH_ROW (adw_switch_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (vaapi),
                                   "Hardware acceleration");
    adw_action_row_set_subtitle (ADW_ACTION_ROW (vaapi),
                                 "Use VAAPI / NVDEC when available");
    adw_switch_row_set_active (vaapi, TRUE);
    adw_preferences_group_add (grp, GTK_WIDGET (vaapi));

    AdwSwitchRow *deint = ADW_SWITCH_ROW (adw_switch_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (deint),
                                   "Deinterlace");
    adw_preferences_group_add (grp, GTK_WIDGET (deint));

    adw_preferences_page_add (page, grp);
    return page;
}

/* Band-centre frequencies for 10-band EQ */
static const gchar *eq_band_labels[] = {
    "31 Hz", "63 Hz", "125 Hz", "250 Hz", "500 Hz",
    "1 kHz", "2 kHz", "4 kHz", "8 kHz", "16 kHz"
};

static const gchar *eq_preset_names[] = {
    "Flat", "Bass Boost", "Treble Boost", "Loudness",
    "Vocal Presence", "Classical", "Pop", "Rock", "Dance", NULL
};
/* dB values for each band in each preset */
static const gdouble eq_presets[][10] = {
    { 0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },  /* Flat */
    { 6,  5,  4,  2,  0, -1, -1, -1, -1, -1 },  /* Bass Boost */
    {-1, -1, -1, -1,  0,  1,  2,  4,  5,  6 },  /* Treble Boost */
    { 5,  4,  0,  0, -2,  0,  0,  2,  4,  5 },  /* Loudness */
    { 0,  0,  0,  2,  4,  5,  4,  2,  0,  0 },  /* Vocal Presence */
    { 4,  3,  2,  0, -2,  0,  0,  0,  3,  5 },  /* Classical */
    { 1,  1,  0,  1,  3,  3,  2,  1,  1,  1 },  /* Pop */
    { 4,  3,  2, -1, -2,  0,  2,  3,  3,  2 },  /* Rock */
    { 4,  5,  3,  0, -2, -2,  0,  3,  4,  4 },  /* Dance */
};

typedef struct { GSettings *settings; guint band; } BandData;

static void
on_band_changed (GtkAdjustment *adj, gpointer user_data)
{
    BandData   *bd    = user_data;
    GSettings  *s     = bd->settings;
    GVariant   *old   = g_settings_get_value (s, "equalizer-gains");
    gsize       n     = 0;
    const gdouble *arr = g_variant_get_fixed_array (old, &n, sizeof(gdouble));
    gdouble gains[10];
    for (gsize i = 0; i < 10; i++) gains[i] = (i < n) ? arr[i] : 0.0;
    gains[bd->band] = gtk_adjustment_get_value (adj);
    g_settings_set_value (s, "equalizer-gains",
        g_variant_new_fixed_array (G_VARIANT_TYPE_DOUBLE, gains, 10, sizeof(gdouble)));
    g_variant_unref (old);
}

static void
on_preset_changed (AdwComboRow *row, GParamSpec *pspec, gpointer user_data)
{
    (void) pspec;
    gpointer *ptrs = user_data;          /* [0]=settings [1..10]=adjs */
    GSettings *s   = ptrs[0];
    guint      sel = adw_combo_row_get_selected (row);
    if (sel >= G_N_ELEMENTS (eq_presets)) return;
    const gdouble *preset = eq_presets[sel];
    /* Update setting */
    g_settings_set_value (s, "equalizer-gains",
        g_variant_new_fixed_array (G_VARIANT_TYPE_DOUBLE, preset, 10, sizeof(gdouble)));
    /* Update sliders without triggering on_band_changed re-entrance */
    for (guint i = 0; i < 10; i++) {
        GtkAdjustment *adj = GTK_ADJUSTMENT (ptrs[i + 1]);
        g_signal_handlers_block_matched (adj, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                         on_band_changed, NULL);
        gtk_adjustment_set_value (adj, preset[i]);
        g_signal_handlers_unblock_matched (adj, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                           on_band_changed, NULL);
    }
    /* Store preset name */
    g_settings_set_string (s, "equalizer-preset", eq_preset_names[sel]);
}

static void
on_image_reset_clicked (GtkButton *btn, gpointer user_data)
{
    GSettings *settings = user_data;
    (void) btn;
    g_settings_reset (settings, "video-brightness");
    g_settings_reset (settings, "video-contrast");
    g_settings_reset (settings, "video-saturation");
    g_settings_reset (settings, "video-hue");
}

static AdwPreferencesPage *
build_image_page (void)
{
    GSettings *settings = g_settings_new ("io.github.velox");

    AdwPreferencesPage  *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
    adw_preferences_page_set_title    (page, "Image");
    adw_preferences_page_set_icon_name (page, "image-x-generic-symbolic");

    AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (grp, "Adjustments");

    struct {
        const gchar *key;
        const gchar *title;
        gdouble min, max, step;
    } sliders[] = {
        { "video-brightness", "Brightness", -1.0, 1.0, 0.05 },
        { "video-contrast",   "Contrast",    0.0, 2.0, 0.05 },
        { "video-saturation", "Saturation",  0.0, 2.0, 0.05 },
        { "video-hue",        "Hue",        -1.0, 1.0, 0.05 },
    };

    for (guint i = 0; i < G_N_ELEMENTS (sliders); i++) {
        AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), sliders[i].title);

        GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                                     sliders[i].min, sliders[i].max, sliders[i].step);
        gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
        gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
        
        /* Add a center mark for default value */
        gdouble def_val = (g_strcmp0(sliders[i].key, "video-contrast") == 0 || 
                           g_strcmp0(sliders[i].key, "video-saturation") == 0) ? 1.0 : 0.0;
        gtk_scale_add_mark (GTK_SCALE (scale), def_val, GTK_POS_TOP, NULL);
        
        gtk_widget_set_size_request (scale, 200, -1);
        gtk_widget_set_valign (scale, GTK_ALIGN_CENTER);
        
        g_settings_bind (settings, sliders[i].key,
                         gtk_range_get_adjustment (GTK_RANGE (scale)), "value",
                         G_SETTINGS_BIND_DEFAULT);

        adw_action_row_add_suffix (row, scale);
        adw_preferences_group_add (grp, GTK_WIDGET (row));
    }

    /* Reset button */
    AdwActionRow *reset_row = ADW_ACTION_ROW (adw_action_row_new ());
    GtkWidget *reset_btn = gtk_button_new_with_label ("Reset to defaults");
    gtk_widget_set_valign (reset_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (reset_btn, "destructive-action");
    g_signal_connect (reset_btn, "clicked", G_CALLBACK (on_image_reset_clicked), settings);
    adw_action_row_add_suffix (reset_row, reset_btn);
    adw_preferences_group_add (grp, GTK_WIDGET (reset_row));

    adw_preferences_page_add (page, grp);
    return page;
}

static AdwPreferencesPage *
build_audio_page (void)
{
    GSettings *settings = g_settings_new ("io.github.velox");

    AdwPreferencesPage  *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
    adw_preferences_page_set_title    (page, "Audio");
    adw_preferences_page_set_icon_name (page, "audio-speakers-symbolic");

    /* ── Equalizer group ─────────────────────────────────────────────── */
    AdwPreferencesGroup *eq_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title       (eq_grp, "Equalizer");
    adw_preferences_group_set_description (eq_grp, "Boost or cut individual frequency bands");

    /* Preset row */
    AdwComboRow *preset_row = ADW_COMBO_ROW (adw_combo_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (preset_row), "Preset");
    GtkStringList *presets_list = gtk_string_list_new (eq_preset_names);
    adw_combo_row_set_model (preset_row, G_LIST_MODEL (presets_list));
    adw_preferences_group_add (eq_grp, GTK_WIDGET (preset_row));

    /* Recover current gains */
    GVariant   *gains_var = g_settings_get_value (settings, "equalizer-gains");
    gsize       n_gains   = 0;
    const gdouble *gains  = g_variant_get_fixed_array (gains_var, &n_gains, sizeof(gdouble));

    /* Closure data: [0]=settings, [1..10]=adjustments */
    gpointer *closure = g_new0 (gpointer, 12);
    closure[0] = settings;

    /* One scale row per band */
    for (guint i = 0; i < 10; i++) {
        gdouble initial = (i < n_gains) ? gains[i] : 0.0;

        GtkAdjustment *adj = gtk_adjustment_new (initial, -12.0, 12.0, 0.5, 1.0, 0.0);
        closure[i + 1] = adj;

        BandData *bd    = g_new (BandData, 1);   /* leaked intentionally; dialog lifetime */
        bd->settings    = settings;
        bd->band        = i;
        g_signal_connect (adj, "value-changed", G_CALLBACK (on_band_changed), bd);

        AdwActionRow *band_row = ADW_ACTION_ROW (adw_action_row_new ());
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (band_row),
                                       eq_band_labels[i]);

        /* Vertical scale in a box so it fits cleanly */
        GtkWidget *scale = gtk_scale_new (GTK_ORIENTATION_HORIZONTAL, adj);
        gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
        gtk_scale_set_value_pos  (GTK_SCALE (scale), GTK_POS_RIGHT);
        gtk_scale_add_mark (GTK_SCALE (scale), 0, GTK_POS_TOP, NULL);
        gtk_widget_set_size_request (scale, 200, -1);
        gtk_widget_set_valign (scale, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix (band_row, scale);
        adw_preferences_group_add (eq_grp, GTK_WIDGET (band_row));
    }
    g_variant_unref (gains_var);

    /* Wire up preset selector *after* adjustments are created */
    g_signal_connect (preset_row, "notify::selected",
                      G_CALLBACK (on_preset_changed), closure);

    /* Set initial preset selection from saved string */
    gchar *saved_preset = g_settings_get_string (settings, "equalizer-preset");
    for (guint i = 0; eq_preset_names[i]; i++) {
        if (g_strcmp0 (saved_preset, eq_preset_names[i]) == 0) {
            adw_combo_row_set_selected (preset_row, i);
            break;
        }
    }
    g_free (saved_preset);

    adw_preferences_page_add (page, eq_grp);

    /* ── Audio sync group ────────────────────────────────────────────── */
    AdwPreferencesGroup *sync_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (sync_grp, "Synchronisation");

    AdwSpinRow *delay_row = ADW_SPIN_ROW (
        adw_spin_row_new_with_range (-5000, 5000, 50));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (delay_row),
                                   "Audio delay (ms)");
    adw_action_row_set_subtitle (ADW_ACTION_ROW (delay_row),
                                 "Negative = advance audio, positive = delay");
    adw_preferences_group_add (sync_grp, GTK_WIDGET (delay_row));

    AdwSwitchRow *norm_row = ADW_SWITCH_ROW (adw_switch_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (norm_row),
                                   "Volume normalisation");
    adw_action_row_set_subtitle (ADW_ACTION_ROW (norm_row),
                                 "Level audio across tracks (ReplayGain)");
    adw_preferences_group_add (sync_grp, GTK_WIDGET (norm_row));

    adw_preferences_page_add (page, sync_grp);

    return page;
}

static AdwPreferencesPage *
build_subtitles_page (void)
{
    AdwPreferencesPage  *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
    adw_preferences_page_set_title    (page, "Subtitles");
    adw_preferences_page_set_icon_name (page, "media-view-subtitles-symbolic");

    AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (grp, "Style");

    AdwEntryRow *font_row = ADW_ENTRY_ROW (adw_entry_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (font_row),
                                   "Font");
    gtk_editable_set_text (GTK_EDITABLE (font_row), "Sans Bold 20");
    adw_preferences_group_add (grp, GTK_WIDGET (font_row));

    AdwSpinRow *size_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (8, 72, 1));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (size_row), "Size");
    adw_spin_row_set_value (size_row, 20);
    adw_preferences_group_add (grp, GTK_WIDGET (size_row));

    AdwSpinRow *bg_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (0, 100, 5));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (bg_row),
                                   "Background opacity (%)");
    adw_spin_row_set_value (bg_row, 65);
    adw_preferences_group_add (grp, GTK_WIDGET (bg_row));

    adw_preferences_page_add (page, grp);
    return page;
}

static AdwPreferencesPage *
build_interface_page (void)
{
    AdwPreferencesPage  *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
    adw_preferences_page_set_title    (page, "Interface");
    adw_preferences_page_set_icon_name (page, "preferences-desktop-symbolic");

    AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (grp, "Appearance");

    /* Dark mode */
    AdwComboRow *theme = ADW_COMBO_ROW (adw_combo_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (theme), "Colour scheme");
    GtkStringList *themes = gtk_string_list_new (
        (const char *[]) { "System", "Light", "Dark", NULL });
    adw_combo_row_set_model (theme, G_LIST_MODEL (themes));
    adw_combo_row_set_selected (theme, 2);   /* Dark by default */
    adw_preferences_group_add (grp, GTK_WIDGET (theme));

    /* OSD timeout */
    AdwSpinRow *osd = ADW_SPIN_ROW (adw_spin_row_new_with_range (500, 10000, 500));
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (osd),
                                   "Controls auto-hide delay (ms)");
    adw_spin_row_set_value (osd, 2500);
    adw_preferences_group_add (grp, GTK_WIDGET (osd));

    adw_preferences_page_add (page, grp);
    return page;
}

static void
vlx_settings_dialog_class_init (VlxSettingsDialogClass *klass)
{
    (void) klass;
}

static void
vlx_settings_dialog_init (VlxSettingsDialog *self)
{
    adw_dialog_set_title (ADW_DIALOG (self), "Preferences");
    adw_dialog_set_content_width  (ADW_DIALOG (self), 600);
    adw_dialog_set_content_height (ADW_DIALOG (self), 500);

    adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (self),
                                build_playback_page ());
    adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (self),
                                build_video_page ());
    adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (self),
                                build_image_page ());
    adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (self),
                                build_audio_page ());
    adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (self),
                                build_subtitles_page ());
    adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (self),
                                build_interface_page ());
}

VlxSettingsDialog *
vlx_settings_dialog_new (void)
{
    return g_object_new (VLX_TYPE_SETTINGS_DIALOG, NULL);
}
