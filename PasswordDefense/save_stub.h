// save_stub.h — Full cfg:: interface stub for macOS UI compilation
// TODO: replace with ported save.h/save.cpp
#pragma once

#include <string>
#include <vector>

namespace cfg {
    struct ColorRGBA { float r, g, b, a; };

    struct DetailedHeaderColumns {
        bool title    = true;
        bool username = true;
        bool email    = true;
        bool date     = true;
        bool index    = true;
        bool pin_fav  = true;
        bool sub_password = true;
        bool sub_card     = true;
        bool sub_identity = true;
        bool sub_note     = false;
    };

    // Theme
    int get_theme();
    void set_theme(int theme);
    bool is_dark_theme();

    // Colors
    ColorRGBA get_soft_container_dark();
    ColorRGBA get_soft_container_light();
    ColorRGBA get_card_bg_dark();
    ColorRGBA get_card_bg_light();
    ColorRGBA get_card_header_bg_dark();
    ColorRGBA get_card_header_bg_light();
    ColorRGBA get_card_body_bg_dark();
    ColorRGBA get_card_body_bg_light();
    ColorRGBA get_controls_bg_dark();
    ColorRGBA get_controls_bg_light();
    ColorRGBA get_window_bg_dark();
    ColorRGBA get_window_bg_light();

    void set_soft_container_dark(float r, float g, float b, float a);
    void set_soft_container_light(float r, float g, float b, float a);
    void set_card_bg_dark(float r, float g, float b, float a);
    void set_card_bg_light(float r, float g, float b, float a);
    void set_card_header_bg_dark(float r, float g, float b, float a);
    void set_card_header_bg_light(float r, float g, float b, float a);
    void set_card_body_bg_dark(float r, float g, float b, float a);
    void set_card_body_bg_light(float r, float g, float b, float a);
    void set_controls_bg_dark(float r, float g, float b, float a);
    void set_controls_bg_light(float r, float g, float b, float a);
    void set_window_bg_dark(float r, float g, float b, float a);
    void set_window_bg_light(float r, float g, float b, float a);

    // View & sorting
    int  get_view_mode();
    void set_view_mode(int mode);
    int  get_order_key();
    void set_order_key(int key);
    int  get_order_dir();
    void set_order_dir(int dir);
    int  get_group_mode();
    void set_group_mode(int mode);

    // Pill tabs
    std::string get_pill_tab_0();
    void set_pill_tab_0(const std::string& label);
    std::string get_pill_tab_1();
    void set_pill_tab_1(const std::string& label);

    // Behavior
    bool get_hover_expand();
    void set_hover_expand(bool enabled);
    bool get_show_group_count();
    void set_show_group_count(bool enabled);
    int  get_row_gap();
    void set_row_gap(int gap);
    bool get_autosave_enabled();
    void set_autosave_enabled(bool enabled);
    bool get_autoscroll_enabled();
    void set_autoscroll_enabled(bool enabled);
    bool get_always_on_top();
    void set_always_on_top(bool enabled);
    bool get_minimize_to_tray();
    void set_minimize_to_tray(bool enabled);
    bool get_start_on_boot();
    void set_start_on_boot(bool enabled);
    bool get_start_minimized();
    void set_start_minimized(bool enabled);
    bool get_auto_open_vault();
    void set_auto_open_vault(bool enabled);

    // Backup
    bool get_auto_backup();
    void set_auto_backup(bool enabled);
    int  get_backup_keep_count();
    void set_backup_keep_count(int count);

    // Security
    int get_clipboard_clear_delay();
    void set_clipboard_clear_delay(int seconds);
    int get_auto_lock_timeout();
    void set_auto_lock_timeout(int seconds);
    bool get_reprompt_reveal_password();
    void set_reprompt_reveal_password(bool enabled);
    bool get_reprompt_export();
    void set_reprompt_export(bool enabled);
    bool get_reprompt_disable_readonly();
    void set_reprompt_disable_readonly(bool enabled);
    bool get_reprompt_reveal_notes();
    void set_reprompt_reveal_notes(bool enabled);
    int  get_reprompt_lockout_count();
    void set_reprompt_lockout_count(int count);
    int  get_self_destruct_mode();
    void set_self_destruct_mode(int mode);
    int  get_password_max_age_days();
    void set_password_max_age_days(int days);
    int  get_trash_retention_days();
    void set_trash_retention_days(int days);

    // Font / privacy
    float get_font_scale();
    void set_font_scale(float scale);
    bool get_privacy_mode();
    void set_privacy_mode(bool enabled);
    bool get_blur_on_unfocus();
    void set_blur_on_unfocus(bool enabled);

    // Recently used credentials
    std::string get_recent_items_json();
    void set_recent_items_json(const std::string& json_array);

    // Detailed header columns
    DetailedHeaderColumns get_detailed_header_columns();
    void set_detailed_header_columns(const DetailedHeaderColumns& cols);

    // One-shot cleanup for legacy localhost-HTTP keys (no-op in the stub).
    void migrate_remove_legacy_local_server_keys();

    // Three-pane
    bool get_three_pane_sidebar_collapsed();
    void set_three_pane_sidebar_collapsed(bool collapsed);
    bool get_three_pane_list_collapsed();
    void set_three_pane_list_collapsed(bool collapsed);
    bool get_three_pane_sidebar_auto_collapse();
    void set_three_pane_sidebar_auto_collapse(bool enabled);
    bool get_three_pane_list_auto_collapse();
    void set_three_pane_list_auto_collapse(bool enabled);
}
