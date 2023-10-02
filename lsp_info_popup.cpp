#include <string>
#include <iostream>
#include <sstream>
#include <map>
#include <vector>
#include <memory>

using namespace std;

#include "json.hpp"
using json = nlohmann::json;

extern "C" {
#include <yed/plugin.h>
}

#define DEFAULT_THRESHOLD  1500
#define REQUEST_TIMEOUT    4000

static yed_plugin         *Self;
static unsigned long long  cursor_idle_start_ms;
static unsigned long long  cursor_is_idle;
static int                 requested;
static unsigned long long  request_time;


static string uri_for_buffer(yed_buffer *buffer) {
    string uri = "";

    if (!(buffer->flags & BUFF_SPECIAL)
    &&  buffer->kind == BUFF_KIND_FILE) {
        if (buffer->path == NULL) {
            uri += "untitled:";
            uri += buffer->name;
        } else {
            uri += "file://";
            uri += buffer->path;
        }
    }

    return uri;
}

struct Position {
    size_t line;
    size_t character;

    Position(size_t line, size_t character) : line(line), character(character) {}
};

struct String_Entry {
    string text;
    int    yed_ft;

    String_Entry(string &&text)             : text(std::move(text)), yed_ft(-1)     {}
    String_Entry(string &&text, int yed_ft) : text(std::move(text)), yed_ft(yed_ft) {}
};

struct Popup {
    struct Popup_Text {
        string text;
        int    yed_ft;
    };

    vector<Popup_Text> texts;
    vector<string>     lines;
    array_t            line_attrs;
    int                row;
    int                byte;
    int                max_width;
    int                guess_ft;

    Popup(int row, int byte, int guess_ft) : row(row), byte(byte), guess_ft(guess_ft), max_width(0) {
        this->line_attrs = array_make(array_t);
    }

    ~Popup() {
        array_t *it;

        array_traverse(this->line_attrs, it) {
            array_free(*it);
        }

        array_free(this->line_attrs);
    }

    void add_text(string &&text, int yed_ft = FT_UNKNOWN) {
        this->texts.emplace_back(Popup_Text{ std::move(text), yed_ft });
    }

    void finish() {
        int fallback_ft = FT_UNKNOWN;

        for (const auto &t : this->texts) {
            if (t.yed_ft != FT_UNKNOWN) {
                goto dont_guess_ft;
            }
        }

        fallback_ft = this->guess_ft;
dont_guess_ft:;

        for (const auto &t : this->texts) {
            const auto &text  = t.text;
            int        yed_ft = t.yed_ft >= 0 ? t.yed_ft : fallback_ft;

            yed_event event;

            event.kind                  = EVENT_HIGHLIGHT_REQUEST;
            event.highlight_string      = text.c_str();
            event.highlight_lines_attrs = array_make(array_t);
            event.ft                    = yed_ft;

            if (yed_ft >= 0) {
                yed_trigger_event(&event);
            }

            istringstream is(text);

            string line;
            for (int l = 0; getline(is, line); l += 1) {
                int width = yed_get_string_width(line.c_str());
                if (width > this->max_width) { this->max_width = width; }

                if (l < array_len(event.highlight_lines_attrs)) {
                    array_push(this->line_attrs, *(array_t*)array_item(event.highlight_lines_attrs, l));
                } else {
                    array_t col_attrs = array_make(yed_attrs);
                    for (int i = 0; i < width; i += 1) {
                        yed_attrs a = ZERO_ATTR;
                        array_push(col_attrs, a);
                    }
                    array_push(this->line_attrs, col_attrs);
                }

                this->lines.push_back(line);
            }

            array_free(event.highlight_lines_attrs);
        }
    }
};

static Position position_in_frame(yed_frame *frame) {
    yed_line *line;

    if (frame == NULL || frame->buffer == NULL) {
        return Position(-1, -1);
    }

    line = yed_buff_get_line(frame->buffer, frame->cursor_line);
    if (line == NULL) {
        return Position(-1, -1);
    }

    return Position(frame->cursor_line - 1, yed_line_col_to_idx(line, frame->cursor_col));
}

static map<string, string> ft_map = {
    { "abap",            "ABAP"             },
    { "bat",             "Windows Bat"      },
    { "bibtex",          "BibTeX"           },
    { "clojure",         "Clojure"          },
    { "coffeescript",    "Coffeescript"     },
    { "c",               "C"                },
    { "cpp",             "C++"              },
    { "csharp",          "C#"               },
    { "Diff",            "diff"             },
    { "dart",            "Dart"             },
    { "dockerfile",      "Dockerfile"       },
    { "elixir",          "Elixir"           },
    { "erlang",          "Erlang"           },
    { "fsharp",          "F#"               },
    { "git-commit",      "Git"              },
    { "go",              "Go"               },
    { "groovy",          "Groovy"           },
    { "handlebars",      "Handlebars"       },
    { "html",            "HTML"             },
    { "ini",             "Ini"              },
    { "java",            "Java"             },
    { "javascript",      "JavaScript"       },
    { "javascriptreact", "JavaScript React" },
    { "json",            "JSON"             },
    { "latex",           "LaTeX"            },
    { "less",            "Less"             },
    { "lua",             "Lua"              },
    { "makefile",        "Make"             },
    { "markdown",        "Markdown"         },
    { "objective-c",     "Objective-C"      },
    { "objective-cpp",   "Objective-C++"    },
    { "perl",            "Perl"             },
    { "perl6",           "Perl 6"           },
    { "php",             "PHP"              },
    { "powershell",      "Powershell"       },
    { "jade",            "Pug"              },
    { "python",          "Python"           },
    { "r",               "R"                },
    { "razor",           "Razor (cshtml)"   },
    { "ruby",            "Ruby"             },
    { "rust",            "Rust"             },
    { "sass",            "SCSS"             },
    { "scala",           "Scala"            },
    { "shaderlab",       "ShaderLab"        },
    { "shellscript",     "Shell"            },
    { "sql",             "SQL"              },
    { "swift",           "Swift"            },
    { "typescript",      "TypeScript"       },
    { "typescriptreact", "TypeScript React" },
    { "tex",             "TeX"              },
    { "vb",              "Visual Basic"     },
    { "xml",             "XML"              },
    { "xsl",             "XSL"              },
    { "yaml",            "YAML"             },
};

static unique_ptr<Popup> popup;


static void request(yed_frame *frame) {
    requested = 1;

    request_time = measure_time_now_ms();

    if (frame == NULL
    ||  frame->buffer == NULL
    ||  frame->buffer->kind != BUFF_KIND_FILE
    ||  frame->buffer->flags & BUFF_SPECIAL) {

        return;
    }

    string   uri = uri_for_buffer(frame->buffer);
    Position pos = position_in_frame(frame);

    json params = {
        { "textDocument", {
            { "uri", uri },
        }},
        { "position", {
            { "line",      pos.line      },
            { "character", pos.character },
        }},
    };


    yed_event event;
    string    text = params.dump();

    event.kind                       = EVENT_PLUGIN_MESSAGE;
    event.plugin_message.message_id  = "lsp-request:textDocument/hover";
    event.plugin_message.plugin_id   = "lsp_info_popup";
    event.plugin_message.string_data = text.c_str();
    event.ft                         = frame->buffer->ft;

    yed_trigger_event(&event);
}

static void pump(yed_event *event) {
    unsigned long long now;
    int                cursor_idle_threshold_ms;
    int                cursor_was_idle;

    now = measure_time_now_ms();

    if (now - request_time > REQUEST_TIMEOUT) {
        requested = 0;
    }

    if (!yed_get_var_as_int("lsp-info-popup-idle-threshold-ms", &cursor_idle_threshold_ms)) {
        cursor_idle_threshold_ms = DEFAULT_THRESHOLD;
    }

    if (cursor_idle_threshold_ms < 0) { return; }

    cursor_was_idle = cursor_is_idle;
    cursor_is_idle  = now - cursor_idle_start_ms >= cursor_idle_threshold_ms;

    if (!cursor_was_idle && cursor_is_idle && !popup && !requested) {
        request(ys->active_frame);
    }

}

static void pmsg(yed_event *event) {
    if (strcmp(event->plugin_message.plugin_id, "lsp") != 0
    ||  strcmp(event->plugin_message.message_id, "textDocument/hover") != 0) {
        return;
    }

    if (!requested) { return; }

    try {
        auto j = json::parse(event->plugin_message.string_data);
        const auto &result = j["result"];

        int row  = 0;
        int byte = 0;
        if (result.contains("range")) {
            const auto &range = result["range"];
            row = range["start"]["line"];
            row += 1;
            byte = range["start"]["character"];
        } else if (ys->active_frame != NULL) {
            row = ys->active_frame->cursor_line;
            if (ys->active_frame->buffer != NULL) {
                yed_line *line = yed_buff_get_line(ys->active_frame->buffer, row);
                if (line != NULL) {
                    byte = yed_line_col_to_idx(line, ys->active_frame->cursor_col);
                }
            }
        }

        int guess_ft = FT_UNKNOWN;
        if (ys->active_frame != NULL && ys->active_frame->buffer != NULL) {
            guess_ft = ys->active_frame->buffer->ft;
        }

        popup = unique_ptr<Popup>(new Popup(row, byte, guess_ft));

        const json &contents = result["contents"];

        auto handle_one = [&](const json &item) {
            if (item.is_string()) {
                popup->add_text(item);
            } else if (item.is_object()) {
                if (item.contains("language")) {
                    int yed_ft = -1;
                    auto search = ft_map.find(item["language"]);
                    if (search != ft_map.end()) {
                        yed_ft = yed_get_ft((char*)search->second.c_str());
                        if (yed_ft == FT_ERR_NOT_FOUND) {
                            yed_ft = FT_UNKNOWN;
                        }
                    }
                    popup->add_text(item["value"], yed_ft);
                } else {
                    popup->add_text(item["value"], FT_UNKNOWN);
                }
            }
        };

        if (contents.is_array()) {
            for (const auto &item : contents) {
                handle_one(item);
            }
        } else {
            handle_one(contents);
        }

        popup->finish();
    } catch (...) {}

    if (popup) {
        requested = 0;
    }

    event->cancel = 1;
}

static void move(yed_event *event) {
    cursor_is_idle = 0;
    cursor_idle_start_ms = measure_time_now_ms();
    popup.reset();
}

static void bmod(yed_event *event) {
    if (ys->active_frame         == NULL
    ||  ys->active_frame->buffer == NULL
    ||  ys->active_frame->buffer != event->buffer) {

        return;
    }

    cursor_is_idle = 0;
    cursor_idle_start_ms = measure_time_now_ms();
    popup.reset();
}

static void draw(yed_event *event) {
    if (!popup || popup->lines.size() == 0) { return; }

    if (ys->active_frame == NULL || ys->active_frame->buffer == NULL) { return; }

    yed_frame *f = ys->active_frame;
    int        y = yed_frame_line_to_y(f, popup->row);
    if (y <= 0) { return; }

    yed_line *line = yed_buff_get_line(f->buffer, popup->row);
    if (line == NULL) { return; }

    int col = yed_line_idx_to_col(line, popup->byte);
    if (col <= f->buffer_x_offset || col > f->buffer_x_offset + f->width) { return; }

    int x = f->left + col - (f->buffer_x_offset + 1);

    yed_attrs attrs = ZERO_ATTR;
    if (yed_active_style_get_popup().flags) {
        attrs = yed_parse_attrs("&active &popup");
    } else {
        attrs = yed_parse_attrs("&active &associate");
    }

    int n_lines     = popup->lines.size();
    int extra_lines = 2;
    int width       = popup->max_width;

    if (y > n_lines + extra_lines) {
        y -= n_lines + extra_lines;
    } else {
        y += 1;
    }

    yed_set_attr(attrs);

    for (int i = 0; i < n_lines + extra_lines; i += 1) {
        yed_set_cursor(y + i, x);
        if (i == 0) {
            yed_screen_print_over("┌");
        } else if (i == n_lines + extra_lines - 1) {
            yed_screen_print_over("└");
        } else {
            yed_screen_print_over("│");
        }
        for (int j = 1; j < width + 1; j += 1) {
            yed_set_cursor(y + i, x + j);
            if (i == 0 || i == n_lines + extra_lines - 1) {
                yed_screen_print_over("─");
            }
        }
        yed_set_cursor(y + i, x + width + 1);
        if (i == 0) {
            yed_screen_print_over("┐");
        } else if (i == n_lines + extra_lines - 1) {
            yed_screen_print_over("┘");
        } else {
            yed_screen_print_over("│");
        }
    }

    y += 1;

    int l = 0;
    for (const auto & line : popup->lines) {
        yed_glyph *git;
        int        c = 1;
        array_t   *line_attrs = (array_t*)array_item(popup->line_attrs, l);

        yed_glyph_traverse(line.c_str(), git) {
            yed_set_cursor(y, x + c);

            yed_attrs a = attrs;
            yed_combine_attrs(&a, (yed_attrs*)array_item(*line_attrs, c - 1));
            yed_set_attr(a);
            yed_screen_print_n_over(&git->c, yed_get_glyph_len(*git));

            c += yed_get_glyph_width(*git);
        }

        for (; c < width + 1; c += 1) {
            yed_set_cursor(y, x + c);
            yed_set_attr(attrs);
            yed_screen_print_n_over(" ", 1);
        }

        y += 1;
        l += 1;
    }
}


static void unload(yed_plugin *self) { }

static void lsp_info(int n_args, char **args) {
    if (ys->active_frame         == NULL
    ||  ys->active_frame->buffer == NULL) {

        return;
    }

    request(ys->active_frame);
}

extern "C"
int yed_plugin_boot(yed_plugin *self) {
    YED_PLUG_VERSION_CHECK();

    Self = self;

    map<void(*)(yed_event*), vector<yed_event_kind_t> > event_handlers = {
        { pump,     { EVENT_POST_PUMP         } },
        { pmsg,     { EVENT_PLUGIN_MESSAGE    } },
        { move,     { EVENT_CURSOR_POST_MOVE  } },
        { bmod,     { EVENT_BUFFER_POST_MOD   } },
        { draw,     { EVENT_PRE_DIRECT_DRAWS  } },
    };

    map<const char*, const char*> vars          = { { "lsp-info-popup-idle-threshold-ms", XSTR(DEFAULT_THRESHOLD) } };
    map<const char*, void(*)(int, char**)> cmds = { { "lsp-info", lsp_info} };

    for (auto &pair : event_handlers) {
        for (auto evt : pair.second) {
            yed_event_handler h;
            h.kind = evt;
            h.fn   = pair.first;
            yed_plugin_add_event_handler(self, h);
        }
    }

    for (auto &pair : vars) {
        if (!yed_get_var(pair.first)) { yed_set_var(pair.first, pair.second); }
    }

    for (auto &pair : cmds) {
        yed_plugin_set_command(self, pair.first, pair.second);
    }

    cursor_idle_start_ms = measure_time_now_ms();

    /* Fake cursor move so that it works on startup/reload. */
    yed_move_cursor_within_active_frame(0, 0);

    yed_plugin_set_unload_fn(self, unload);

    return 0;
}
