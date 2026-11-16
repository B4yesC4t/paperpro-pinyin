// pinyin-rime: the Rime input method engine behind a line protocol, for
// libpaperpro-pinyin (and any other program that wants pinyin). One session;
// requests on stdin, one JSON state line on stdout per request.
//
//   key <keysym> <mask>   a key press (X11 keysym, Rime modifier mask); the
//                         reply says whether Rime took it ("handled")
//   pick <i>              candidate i of the current page
//   page <0|1>            previous / next page
//   ascii <0|1>           English (1) or Chinese (0)
//   clear                 drop the composition
//   commit                type the composition's raw input
//
// Reply: {"handled":b,"commit":"…","preedit":"…","cands":["…"],"page":n,
//         "last":b,"ascii":b}
//
// Usage: pinyin-rime SHARED_DIR USER_DIR [PLUGIN.so…]
// Plugins (librime-lua) are loaded before Rime starts: they register their
// modules from a constructor, so dlopen is all it takes.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rime_api.h>

static RimeApi *rime;
static RimeSessionId session;

static void json_str(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void reply(int handled) {
    printf("{\"handled\":%s,\"commit\":", handled ? "true" : "false");
    RIME_STRUCT(RimeCommit, commit);
    if (rime->get_commit(session, &commit)) {
        json_str(commit.text);
        rime->free_commit(&commit);
    } else {
        json_str("");
    }
    RIME_STRUCT(RimeContext, ctx);
    printf(",\"preedit\":");
    if (rime->get_context(session, &ctx)) {
        json_str(ctx.composition.preedit);
        printf(",\"cands\":[");
        for (int i = 0; i < ctx.menu.num_candidates; i++) {
            if (i) putchar(',');
            json_str(ctx.menu.candidates[i].text);
        }
        printf("],\"page\":%d,\"last\":%s", ctx.menu.page_no, ctx.menu.is_last_page ? "true" : "false");
        rime->free_context(&ctx);
    } else {
        printf("\"\",\"cands\":[],\"page\":0,\"last\":true");
    }
    printf(",\"ascii\":%s}\n", rime->get_option(session, "ascii_mode") ? "true" : "false");
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: pinyin-rime SHARED_DIR USER_DIR [PLUGIN.so...]\n");
        return 2;
    }
    for (int i = 3; i < argc; i++)
        if (!dlopen(argv[i], RTLD_NOW | RTLD_GLOBAL)) fprintf(stderr, "pinyin-rime: %s\n", dlerror());

    rime = rime_get_api();
    RIME_STRUCT(RimeTraits, traits);
    traits.shared_data_dir = argv[1];
    traits.user_data_dir = argv[2];
    traits.distribution_name = "PaperPro Pinyin";
    traits.distribution_code_name = "paperpro-pinyin";
    traits.distribution_version = "1";
    traits.app_name = "rime.paperpro-pinyin";
    // Errors only, to stderr (the caller's log), not files in /tmp.
    traits.min_log_level = 2;
    traits.log_dir = "";
    static const char *modules[] = {"default", "lua", NULL};
    traits.modules = modules;
    rime->setup(&traits);
    rime->initialize(&traits);
    // Compiled tables come prebuilt in SHARED_DIR/build; this only checks
    // them (and syncs the user dictionary), so it is quick.
    if (rime->start_maintenance(False)) rime->join_maintenance_thread();
    session = rime->create_session();
    if (!session) {
        fprintf(stderr, "pinyin-rime: no session\n");
        return 1;
    }
    fprintf(stderr, "pinyin-rime: ready\n");
    reply(0);

    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        int a = 0, b = 0;
        if (sscanf(line, "key %d %d", &a, &b) == 2) {
            reply(rime->process_key(session, a, b));
        } else if (sscanf(line, "pick %d", &a) == 1) {
            reply(rime->select_candidate_on_current_page(session, (size_t)a));
        } else if (sscanf(line, "page %d", &a) == 1) {
            reply(rime->change_page(session, a ? False : True));
        } else if (sscanf(line, "ascii %d", &a) == 1) {
            rime->clear_composition(session);
            rime->set_option(session, "ascii_mode", a ? True : False);
            reply(1);
        } else if (strncmp(line, "clear", 5) == 0) {
            rime->clear_composition(session);
            reply(1);
        } else if (strncmp(line, "commit", 6) == 0) {
            reply(rime->commit_composition(session));
        } else {
            reply(0);
        }
    }
    rime->destroy_session(session);
    rime->finalize();
    return 0;
}
