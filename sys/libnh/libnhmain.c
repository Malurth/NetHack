/* NetHack 3.7  libnhmain.c $NHDT-Date: 1693359589 2023/08/30 01:39:49 $  $NHDT-Branch: keni-crashweb2 $:$NHDT-Revision: 1.106 $ */
/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/*-Copyright (c) Robert Patrick Rankin, 2011. */
/* NetHack may be freely redistributed.  See license for details. */

/* main.c - Unix NetHack */

#include "hack.h"
#include "dlb.h"

#include <sys/stat.h>
#include <signal.h>
#include <pwd.h>
#include <func_tab.h>
#ifndef O_RDONLY
#include <fcntl.h>
#endif

/* for cross-compiling to WebAssembly (WASM) */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
void js_helpers_init();
void js_constants_init();
void js_globals_init();
#endif

/* Track whether the game is in position selection mode (getpos).
 * Set to 1 at getpos() entry, cleared at exit.
 * We use a dedicated flag rather than program_state.input_state because
 * Asyncify may unwind the stack before we can read input_state from JS. */
int in_getpos = 0;

/* Global click coordinate buffer for nh_poskey position input.
 * Asyncify saves/restores the C stack on suspend/resume, which means
 * writing to stack-allocated out-pointers from JS (via setValue) gets
 * overwritten when the stack is restored. These globals survive the
 * rewind. The JS callbackRouter writes click coordinates here, and
 * readchar_core copies them to the out-pointers after nh_poskey returns. */
int poskey_click_x = 0;
int poskey_click_y = 0;
int poskey_click_mod = 0;

/* Global select_menu pick_list buffer — same Asyncify workaround as poskey.
 * JS allocates the MENU_ITEM_P array on the WASM heap (via _malloc) and
 * writes the pointer here. shim_select_menu copies it to the C caller's
 * *menu_list out-pointer after Asyncify resumes and the stack is restored. */
MENU_ITEM_P *select_menu_pick_list = NULL;

/* Return pointer to the global so JS can write to it via setValue. */
MENU_ITEM_P **
get_select_menu_pick_list_ptr(void)
{
    return &select_menu_pick_list;
}

/* Return pointers to the click coordinate globals so JS can write to them. */
int *
get_poskey_click_x_ptr(void)
{
    return &poskey_click_x;
}

int *
get_poskey_click_y_ptr(void)
{
    return &poskey_click_y;
}

int *
get_poskey_click_mod_ptr(void)
{
    return &poskey_click_mod;
}

/* Return the current input state.
 * Returns 2 when in position selection mode (farlook, targeting, etc.),
 * 0 otherwise. Values match the InputState enum. */
int
get_input_state(void)
{
    return in_getpos ? 2 : 0;
}

/* Return the player's actual map coordinates (u.ux, u.uy).
 * Unlike the cursor position, these are always the player's true location
 * even during farlook or targeting. */
int
get_player_x(void)
{
    return u.ux;
}

int
get_player_y(void)
{
    return u.uy;
}

/* Return the vision flags at (x,y) from the viz_array.
 * Bit 0 (COULD_SEE=0x1): has line-of-sight if it were lit.
 * Bit 1 (IN_SIGHT=0x2): actually visible (lit + LOS).
 * Bit 2 (TEMP_LIT=0x4): temporarily illuminated.
 * Returns 0 for out-of-bounds. */
int
get_vision_at(int x, int y)
{
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return 0;
    return (int)gv.viz_array[y][x];
}

/* Return whether the tile at (x,y) is in a lit room.
 * Returns: 1 = lit, 0 = dark, -1 = out-of-bounds or unseen. */
int
get_levl_lit(int x, int y)
{
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return -1;
    if (levl[x][y].seenv == 0)
        return -1;
    return levl[x][y].lit ? 1 : 0;
}

/* Return the room number at (x,y) from levl[x][y].roomno.
 * Returns -1 for out-of-bounds or unseen tiles.
 * Room numbers 0-63; corridors and non-rooms are typically 0. */
int
get_levl_roomno(int x, int y)
{
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return -1;
    if (levl[x][y].seenv == 0)
        return -1;
    return (int)levl[x][y].roomno;
}

/* Return the terrain type at (x,y) from levl[x][y].typ.
 * Only returns values for tiles the player has seen (seenv != 0);
 * returns -1 for unseen tiles or out-of-bounds coordinates.
 * Exported to WASM so frontends can identify features beneath other glyphs. */
int
get_levl_typ(int x, int y)
{
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return -1;
    if (levl[x][y].seenv == 0)
        return -1;
    return (int)levl[x][y].typ;
}

/* Return the display color for the terrain at (x,y).
 * Uses back_to_glyph + map_glyphinfo to get the exact same color
 * that print_glyph would use. Returns -1 if invalid. */
int
get_feature_color(int x, int y)
{
    glyph_info fgi = nul_glyphinfo;
    int fglyph;
    boolean prev_use_color;
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return -1;
    /* Temporarily force use_color so the glyphmap contains real colors.
     * In 3.7, colors are baked into the glyphmap cache at init time,
     * so we must rebuild it with use_color=TRUE to get them. */
    prev_use_color = iflags.use_color;
    iflags.use_color = TRUE;
    reset_glyphmap(gm_optionchange);
    fglyph = back_to_glyph(x, y);
    map_glyphinfo(x, y, fglyph, 0, &fgi);
    iflags.use_color = prev_use_color;
    reset_glyphmap(gm_optionchange);
    return fgi.gm.sym.color;
}

/* Fill the provided buffer with rich terrain data for every map tile.
 * Buffer must be at least COLNO * ROWNO * 5 bytes. Layout is row-major,
 * 5 bytes per tile (one struct per (x, y)):
 *   [0] char    — display glyph (back_to_glyph + map_glyphinfo)
 *   [1] color   — NetHack color enum (0-15)
 *   [2] typ     — terrain enum from levl[x][y].typ (e.g. ROOM, DOOR, FOUNTAIN)
 *   [3] vision  — vision flags from gv.viz_array[y][x] (COULD_SEE|IN_SIGHT|TEMP_LIT)
 *   [4] flags   — bit 0 = lit (1=lit, 0=dark), bits 1-7 = roomno (0-63)
 *
 * Index of tile (x, y) is `(y * COLNO + x) * 5`.
 *
 * Char/color come from the BACKGROUND glyph (back_to_glyph) — no
 * monsters/items/effects on top. For unseen tiles (seenv == 0), writes
 * (' ', 0, 0, vision, 0) — the vision byte still reflects current LOS state.
 *
 * Replaces multiple per-tile FFI calls (get_levl_typ, get_vision_at,
 * get_levl_lit, get_levl_roomno, get_feature_color) with one bulk call. */
void
get_terrain_map(unsigned char *out_buffer)
{
    glyph_info ginfo = nul_glyphinfo;
    int g;
    int x, y, idx;
    unsigned char vision;
    boolean prev_use_color;

    if (!out_buffer)
        return;

    /* Match get_feature_color: force use_color and rebuild glyphmap so
     * map_glyphinfo returns the correct ttychar and color. */
    prev_use_color = iflags.use_color;
    iflags.use_color = TRUE;
    reset_glyphmap(gm_optionchange);

    for (y = 0; y < ROWNO; y++) {
        for (x = 0; x < COLNO; x++) {
            idx = (y * COLNO + x) * 5;
            vision = (unsigned char)(gv.viz_array[y][x] & 0xFF);
            if (levl[x][y].seenv == 0) {
                out_buffer[idx]     = ' ';
                out_buffer[idx + 1] = 0;
                out_buffer[idx + 2] = 0;
                out_buffer[idx + 3] = vision;
                out_buffer[idx + 4] = 0;
            } else {
                g = back_to_glyph(x, y);
                map_glyphinfo(x, y, g, 0, &ginfo);
                out_buffer[idx]     = (unsigned char)(ginfo.ttychar & 0xFF);
                out_buffer[idx + 1] = (unsigned char)(ginfo.gm.sym.color & 0xFF);
                out_buffer[idx + 2] = (unsigned char)(levl[x][y].typ & 0xFF);
                out_buffer[idx + 3] = vision;
                /* bit 0 = lit, bits 1-7 = roomno (0-63) */
                out_buffer[idx + 4] = (unsigned char)(
                    (levl[x][y].lit ? 1 : 0) |
                    ((levl[x][y].roomno & 0x7F) << 1)
                );
            }
        }
    }

    iflags.use_color = prev_use_color;
    reset_glyphmap(gm_optionchange);
}

/* Fill out_buffer with glyph data for all floor objects at (x,y).
 * Buffer layout: 8 bytes per object (glyph:i32 LE, ch:u8, color:u8, pad:u16).
 * Returns count of objects written. max_count limits output to prevent overflow.
 * Only returns objects the hero can perceive (at hero's feet, or in sight). */
int
get_floor_objects(int x, int y, unsigned char *out_buffer, int max_count)
{
    struct obj *obj;
    int count = 0;
    glyph_info ginfo = nul_glyphinfo;
    int glyph;
    boolean prev_use_color;

    if (!out_buffer || max_count <= 0)
        return 0;
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return 0;
    if (covers_objects(x, y))
        return 0;
    /* Hero always knows items at own feet; otherwise need vision */
    if (!(u.ux == x && u.uy == y) && !cansee(x, y))
        return 0;

    prev_use_color = iflags.use_color;
    iflags.use_color = TRUE;
    reset_glyphmap(gm_optionchange);

    for (obj = svl.level.objects[x][y]; obj && count < max_count;
         obj = obj->nexthere) {
        int idx = count * 8;
        glyph = obj_to_glyph(obj, rn2);
        map_glyphinfo(x, y, glyph, 0, &ginfo);

        out_buffer[idx]     = (unsigned char)(glyph & 0xFF);
        out_buffer[idx + 1] = (unsigned char)((glyph >> 8) & 0xFF);
        out_buffer[idx + 2] = (unsigned char)((glyph >> 16) & 0xFF);
        out_buffer[idx + 3] = (unsigned char)((glyph >> 24) & 0xFF);
        out_buffer[idx + 4] = (unsigned char)(ginfo.ttychar & 0xFF);
        out_buffer[idx + 5] = (unsigned char)(ginfo.gm.sym.color & 0xFF);
        out_buffer[idx + 6] = 0;
        out_buffer[idx + 7] = 0;
        count++;
    }

    iflags.use_color = prev_use_color;
    reset_glyphmap(gm_optionchange);
    return count;
}

/* Return the clean screen description for position (x,y).
 * Calls do_screen_description() and returns the firstmatch string —
 * the same unambiguous description that auto_describe() displays.
 * Returns empty string for invalid coordinates or no description. */
const char *
get_screen_description(int x, int y)
{
    static char result_buf[BUFSZ];
    coord cc;
    int sym = 0;
    char tmpbuf[BUFSZ];
    const char *firstmatch = "unknown";

    result_buf[0] = '\0';
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return result_buf;

    cc.x = x;
    cc.y = y;
    if (do_screen_description(cc, TRUE, sym, tmpbuf, &firstmatch,
                              (struct permonst **) 0)) {
        strncpy(result_buf, firstmatch, BUFSZ - 1);
        result_buf[BUFSZ - 1] = '\0';
    }
    return result_buf;
}

/* Return a terrain-only description for (x,y) — ignores any monster or
 * object standing on the tile. Wraps dfeature_at(), which inspects
 * levl[x][y].typ and doormask directly, so you get e.g. "closed door",
 * "open door", "doorway", "broken door", "fountain", "altar to <god>",
 * "staircase down", etc. Returns empty string if the tile has no
 * notable dungeon feature (plain floor/wall/etc). */
const char *
get_terrain_description(int x, int y)
{
    static char result_buf[BUFSZ];
    char tmpbuf[BUFSZ];
    const char *desc;

    result_buf[0] = '\0';
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return result_buf;

    desc = dfeature_at((coordxy) x, (coordxy) y, tmpbuf);
    if (desc) {
        strncpy(result_buf, desc, BUFSZ - 1);
        result_buf[BUFSZ - 1] = '\0';
    }
    return result_buf;
}

/* Return the given name of the monster at (x,y), or empty string if none.
 * This is the player-assigned or role-default name (e.g. "Idefix"),
 * not the species name. Works for pets, named monsters, etc.
 * Exported to WASM so frontends can display named creatures. */
const char *
get_monster_givenname(int x, int y)
{
    struct monst *mon;
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return "";
    mon = m_at(x, y);
    if (mon && has_mgivenname(mon))
        return MGIVENNAME(mon);
    return "";
}

/* Return the unique m_id of the monster at (x,y), or 0 if none.
 * m_id is a per-game-session unique identifier assigned at creation;
 * it is never reused, so frontends can use it for stable identity
 * tracking across turns (object permanence). */
unsigned
get_monster_m_id(int x, int y)
{
    struct monst *mon;
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return 0;
    mon = m_at(x, y);
    return mon ? mon->m_id : 0;
}

/* Look up an extended command by name, returning its index in extcmdlist.
 * Returns -1 if not found. Used by the API to resolve command names to
 * the integer index that shim_get_ext_cmd expects. */
int
get_extcmd_index(const char *name)
{
    int i;
    for (i = 0; extcmdlist[i].ef_txt; i++) {
        if (!strcmp(extcmdlist[i].ef_txt, name))
            return i;
    }
    return -1;
}

/* Return stairway/ladder direction at (x,y).
 * Returns: 0 = not stairs, 1 = stairs up, 2 = stairs down,
 *          3 = ladder up, 4 = ladder down.
 * Exported to WASM for the terrain scanner. */
int
get_stair_direction(int x, int y)
{
    stairway *s = stairway_at(x, y);
    if (!s)
        return 0;
    if (s->isladder)
        return s->up ? 3 : 4;
    return s->up ? 1 : 2;
}

/* Return pointer to player's properties array u.uprops[].
 * Each element is a struct prop { long extrinsic, blocked, intrinsic }.
 * Frontends walk the array using PROP indices from js_constants_init. */
struct prop *
get_uprops_ptr(void)
{
    return u.uprops;
}

/* Return the number of properties (LAST_PROP + 1). */
int
get_uprops_count(void)
{
    return LAST_PROP + 1;
}

/* Return a comma-separated string of monster types/species that the
 * player is currently warned about via WARN_OF_MON.  Decodes M2 flags
 * from context.warntype.obj (artifact sources) and .polyd (polymorph
 * sources), plus the specific species (if any).  Returns "" when no
 * warn-of-mon sources are active. */
const char *
get_warntype_text(void)
{
    static char buf[BUFSZ];
    buf[0] = '\0';

    unsigned long combined = svc.context.warntype.obj
                           | svc.context.warntype.polyd;

    /* Decode M2 race/type flags into readable names */
    struct { unsigned long flag; const char *name; } m2names[] = {
        { M2_UNDEAD, "undead" },
        { M2_WERE,   "lycanthropes" },
        { M2_HUMAN,  "humans" },
        { M2_ELF,    "elves" },
        { M2_DWARF,  "dwarves" },
        { M2_GNOME,  "gnomes" },
        { M2_ORC,    "orcs" },
        { M2_DEMON,  "demons" },
        { M2_GIANT,  "giants" },
    };

    int i;
    for (i = 0; i < (int)(sizeof m2names / sizeof m2names[0]); i++) {
        if (combined & m2names[i].flag) {
            if (*buf) Strcat(buf, ",");
            Strcat(buf, m2names[i].name);
        }
    }

    /* Specific species from polymorph (e.g. purple worm warns of shriekers) */
    if (ismnum(svc.context.warntype.speciesidx)) {
        if (*buf) Strcat(buf, ",");
        Strcat(buf, mons[svc.context.warntype.speciesidx].pmnames[NEUTRAL]);
    }

    return buf;
}

#if !defined(_BULL_SOURCE) && !defined(__sgi) && !defined(_M_UNIX)
#if !defined(SUNOS4) && !(defined(ULTRIX) && defined(__GNUC__))
#if defined(POSIX_TYPES) || defined(SVR4) || defined(HPUX)
extern struct passwd *getpwuid(uid_t);
#else
extern struct passwd *getpwuid, (int);
#endif
#endif
#endif
extern struct passwd *getpwnam(const char *);
#ifdef CHDIR
static void chdirx(const char *, boolean);
#endif /* CHDIR */
static boolean whoami(void);
static void process_options(int, char **);

#ifdef _M_UNIX
extern void check_sco_console(void);
extern void init_sco_cons(void);
#endif
#ifdef __linux__
extern void check_linux_console(void);
extern void init_linux_cons(void);
#endif

static void wd_message(void);
static struct passwd *get_unix_pw(void);
ATTRNORETURN static void opt_terminate(void) NORETURN;

#ifdef __EMSCRIPTEN__
/* if WebAssembly, export this API and don't optimize it out */
EMSCRIPTEN_KEEPALIVE
int
main(int argc, char *argv[])

#else /* !__EMSCRIPTEN__ */
int nhmain(int argc, char *argv[]);

int
nhmain(int argc, char *argv[])

#endif /* __EMSCRIPTEN__ */
{
#ifdef CHDIR
    char *dir;
#endif
    NHFILE *nhfp;
    boolean exact_username;
    boolean resuming = FALSE; /* assume new game */
    boolean plsel_once = FALSE;

    early_init(argc, argv);

    gh.hname = argv[0];
    svh.hackpid = getpid();
    (void) umask(0777 & ~FCMASK);

    choose_windows(DEFAULT_WINDOW_SYS);

#ifdef CHDIR /* otherwise no chdir() */
    /*
     * See if we must change directory to the playground.
     * (Perhaps hack runs suid and playground is inaccessible
     *  for the player.)
     * The environment variable HACKDIR is overridden by a
     *  -d command line option (must be the first option given).
     */
    dir = nh_getenv("NETHACKDIR");
    if (!dir)
        dir = nh_getenv("HACKDIR");

    if (argc > 1) {
        if (argcheck(argc, argv, ARG_VERSION) == 2)
            exit(EXIT_SUCCESS);

#ifdef CRASHREPORT
        if (argcheck(argc, argv, ARG_BIDSHOW))
            exit(EXIT_SUCCESS);
#endif

        if (argcheck(argc, argv, ARG_SHOWPATHS) == 2) {
            gd.deferred_showpaths = TRUE;
            return;
        }
        if (argcheck(argc, argv, ARG_DEBUG) == 1) {
            argc--;
            argv++;
        }
        if (argc > 1 && !strncmp(argv[1], "-d", 2) && argv[1][2] != 'e') {
            /* avoid matching "-dec" for DECgraphics; since the man page
             * says -d directory, hope nobody's using -desomething_else
             */
            argc--;
            argv++;
            dir = argv[0] + 2;
            if (*dir == '=' || *dir == ':')
                dir++;
            if (!*dir && argc > 1) {
                argc--;
                argv++;
                dir = argv[0];
            }
            if (!*dir)
                error("Flag -d must be followed by a directory name.");
        }
    }
#endif /* CHDIR */

    if (argc > 1) {
        /*
         * Now we know the directory containing 'record' and
         * may do a prscore().  Exclude `-style' - it's a Qt option.
         */
        if (!strncmp(argv[1], "-s", 2) && strncmp(argv[1], "-style", 6)) {
#ifdef CHDIR
            chdirx(dir, 0);
#endif
#ifdef SYSCF
            initoptions();
#endif
#ifdef PANICTRACE
            ARGV0 = gh.hname; /* save for possible stack trace */
#ifndef NO_SIGNAL
            panictrace_setsignals(TRUE);
#endif
#endif
            prscore(argc, argv);
            /* FIXME: shouldn't this be using nh_terminate() to free
               up any memory allocated by initoptions() */
            exit(EXIT_SUCCESS);
        }
    } /* argc > 1 */

/*
 * Change directories before we initialize the window system so
 * we can find the tile file.
 */
#ifdef CHDIR
    chdirx(dir, 1);
#endif

#ifdef __EMSCRIPTEN__
    js_helpers_init();
    js_constants_init();
    js_globals_init();
#endif

#ifdef _M_UNIX
    check_sco_console();
#endif
#ifdef __linux__
    check_linux_console();
#endif
    initoptions();
#ifdef PANICTRACE
    ARGV0 = gh.hname; /* save for possible stack trace */
#ifndef NO_SIGNAL
    panictrace_setsignals(TRUE);
#endif
#endif
    exact_username = whoami();

    /*
     * It seems you really want to play.
     */
    u.uhp = 1; /* prevent RIP on early quits */
    program_state.preserve_locks = 1;
#ifndef NO_SIGNAL
    sethanguphandler((SIG_RET_TYPE) hangup);
#endif

    process_options(argc, argv); /* command line options */
#ifdef WINCHAIN
    commit_windowchain();
#endif
    init_nhwindows(&argc, argv); /* now we can set up window system */
#ifdef _M_UNIX
    init_sco_cons();
#endif
#ifdef __linux__
    init_linux_cons();
#endif

#ifdef DEF_PAGER
    if (!(gc.catmore = nh_getenv("HACKPAGER"))
        && !(gc.catmore = nh_getenv("PAGER")))
        gc.catmore = DEF_PAGER;
#endif
#ifdef MAIL
    getmailstatus();
#endif

    /* wizard mode access is deferred until here */
    set_playmode(); /* sets plname to "wizard" for wizard mode */
    /* hide any hyphens from plnamesuffix() */
    gp.plnamelen = exact_username ? (int) strlen(svp.plname) : 0;
    /* strip role,race,&c suffix; calls askname() if plname[] is empty
       or holds a generic user name like "player" or "games" */
    plnamesuffix();

    if (wizard) {
        /* use character name rather than lock letter for file names */
        gl.locknum = 0;
    } else {
#ifndef NO_SIGNAL
        /* suppress interrupts while processing lock file */
        (void) signal(SIGQUIT, SIG_IGN);
        (void) signal(SIGINT, SIG_IGN);
#endif
    }

    dlb_init(); /* must be before newgame() */

    /*
     * Initialize the vision system.  This must be before mklev() on a
     * new game or before a level restore on a saved game.
     */
    vision_init();

    init_sound_disp_gamewindows();

    /*
     * First, try to find and restore a save file for specified character.
     * We'll return here if new game player_selection() renames the hero.
     */
 attempt_restore:

    /*
     * getlock() complains and quits if there is already a game
     * in progress for current character name (when gl.locknum == 0)
     * or if there are too many active games (when gl.locknum > 0).
     * When proceeding, it creates an empty <lockname>.0 file to
     * designate the current game.
     * getlock() constructs <lockname> based on the character
     * name (for !gl.locknum) or on first available of alock, block,
     * clock, &c not currently in use in the playground directory
     * (for gl.locknum > 0).
     */
    if (*svp.plname) {
        getlock();
        program_state.preserve_locks = 0; /* after getlock() */
    }

    if (*svp.plname && (nhfp = restore_saved_game()) != 0) {
        const char *fq_save = fqname(gs.SAVEF, SAVEPREFIX, 1);

        (void) chmod(fq_save, 0); /* disallow parallel restores */
#ifndef NO_SIGNAL
        (void) signal(SIGINT, (SIG_RET_TYPE) done1);
#endif
#ifdef NEWS
        if (iflags.news) {
            display_file(NEWS, FALSE);
            iflags.news = FALSE; /* in case dorecover() fails */
        }
#endif
        pline("Restoring save file...");
        mark_synch(); /* flush output */
        if (dorecover(nhfp)) {
            resuming = TRUE; /* not starting new game */
            wd_message();
            if (discover || wizard) {
                /* this seems like a candidate for paranoid_confirmation... */
                if (y_n("Do you want to keep the save file?") == 'n') {
                    (void) delete_savefile();
                } else {
                    (void) chmod(fq_save, FCMASK); /* back to readable */
                    nh_compress(fq_save);
                }
            }
        }
    }

    if (!resuming) {
        boolean neednewlock = (!*svp.plname);
        /* new game:  start by choosing role, race, etc;
           player might change the hero's name while doing that,
           in which case we try to restore under the new name
           and skip selection this time if that didn't succeed */
        if (!iflags.renameinprogress || iflags.defer_plname || neednewlock) {
            if (!plsel_once)
                player_selection();
            plsel_once = TRUE;
            if (neednewlock && *svp.plname)
                goto attempt_restore;
            if (iflags.renameinprogress) {
                /* player has renamed the hero while selecting role;
                   if locking alphabetically, the existing lock file
                   can still be used; otherwise, discard current one
                   and create another for the new character name */
                if (!gl.locknum) {
                    delete_levelfile(0); /* remove empty lock file */
                    getlock();
                }
                goto attempt_restore;
            }
        }
        newgame();
        wd_message();
    }

    /* moveloop() never returns but isn't flagged NORETURN */
    moveloop(resuming);

    exit(EXIT_SUCCESS);
    /*NOTREACHED*/
    return 0;
}

/* caveat: argv elements might be arbitrary long */
static void
process_options(int argc, char *argv[])
{
    int i, l;

    /*
     * Process options.
     */
    while (argc > 1 && argv[1][0] == '-') {
        argv++;
        argc--;
        l = (int) strlen(*argv);
        /* must supply at least 4 chars to match "-XXXgraphics" */
        if (l < 4)
            l = 4;

        switch (argv[0][1]) {
        case 'D':
        case 'd':
            if ((argv[0][1] == 'D' && !argv[0][2])
                || !strcmpi(*argv, "-debug")) {
                wizard = TRUE, discover = FALSE;
            } else if (!strncmpi(*argv, "-DECgraphics", l)) {
                load_symset("DECGraphics", PRIMARYSET);
                switch_symbols(TRUE);
            } else {
                raw_printf("Unknown option: %.60s", *argv);
            }
            break;
        case 'X':
            discover = TRUE, wizard = FALSE;
            break;
#ifdef NEWS
        case 'n':
            iflags.news = FALSE;
            break;
#endif
        case 'u':
            if (argv[0][2]) {
                (void) strncpy(svp.plname, argv[0] + 2, sizeof svp.plname - 1);
                gp.plnamelen = 0; /* plname[] might have -role-race attached */
            } else if (argc > 1) {
                argc--;
                argv++;
                (void) strncpy(svp.plname, argv[0], sizeof svp.plname - 1);
                gp.plnamelen = 0;
            } else {
                raw_print("Player name expected after -u");
            }
            break;
        case 'I':
        case 'i':
            if (!strncmpi(*argv, "-IBMgraphics", l)) {
                load_symset("IBMGraphics", PRIMARYSET);
                load_symset("RogueIBM", ROGUESET);
                switch_symbols(TRUE);
            } else {
                raw_printf("Unknown option: %.60s", *argv);
            }
            break;
        case 'p': /* profession (role) */
            if (argv[0][2]) {
                if ((i = str2role(&argv[0][2])) >= 0)
                    flags.initrole = i;
            } else if (argc > 1) {
                argc--;
                argv++;
                if ((i = str2role(argv[0])) >= 0)
                    flags.initrole = i;
            }
            break;
        case 'r': /* race */
            if (argv[0][2]) {
                if ((i = str2race(&argv[0][2])) >= 0)
                    flags.initrace = i;
            } else if (argc > 1) {
                argc--;
                argv++;
                if ((i = str2race(argv[0])) >= 0)
                    flags.initrace = i;
            }
            break;
        case 'w': /* windowtype */
            config_error_init(FALSE, "command line", FALSE);
            choose_windows(&argv[0][2]);
            config_error_done();
            break;
        case '@':
            flags.randomall = 1;
            break;
        default:
            if ((i = str2role(&argv[0][1])) >= 0) {
                flags.initrole = i;
                break;
            }
            /* else raw_printf("Unknown option: %.60s", *argv); */
        }
    }

#ifdef SYSCF
    if (argc > 1)
        raw_printf("MAXPLAYERS are set in sysconf file.\n");
#else
    /* XXX This is deprecated in favor of SYSCF with MAXPLAYERS */
    if (argc > 1)
        gl.locknum = atoi(argv[1]);
#endif
#ifdef MAX_NR_OF_PLAYERS
    /* limit to compile-time limit */
    if (!gl.locknum || gl.locknum > MAX_NR_OF_PLAYERS)
        gl.locknum = MAX_NR_OF_PLAYERS;
#endif
#ifdef SYSCF
    /* let syscf override compile-time limit */
    if (!gl.locknum || (sysopt.maxplayers && gl.locknum > sysopt.maxplayers))
        gl.locknum = sysopt.maxplayers;
#endif
}

#ifdef CHDIR
static void
chdirx(const char *dir, boolean wr)
{
    if (dir /* User specified directory? */
#ifdef HACKDIR
        && strcmp(dir, HACKDIR) /* and not the default? */
#endif
        ) {
#ifdef SECURE
        (void) setgid(getgid());
        (void) setuid(getuid()); /* Ron Wessels */
#endif
    } else {
        /* non-default data files is a sign that scores may not be
         * compatible, or perhaps that a binary not fitting this
         * system's layout is being used.
         */
#ifdef VAR_PLAYGROUND
        int len = strlen(VAR_PLAYGROUND);

        gf.fqn_prefix[SCOREPREFIX] = (char *) alloc(len + 2);
        Strcpy(gf.fqn_prefix[SCOREPREFIX], VAR_PLAYGROUND);
        if (gf.fqn_prefix[SCOREPREFIX][len - 1] != '/') {
            gf.fqn_prefix[SCOREPREFIX][len] = '/';
            gf.fqn_prefix[SCOREPREFIX][len + 1] = '\0';
        }

#endif
    }

#ifdef HACKDIR
    if (dir == (const char *) 0)
        dir = HACKDIR;
#endif

    if (dir && chdir(dir) < 0) {
        perror(dir);
        error("Cannot chdir to %s.", dir);
    }

    /* warn the player if we can't write the record file
     * perhaps we should also test whether . is writable
     * unfortunately the access system-call is worthless.
     */
    if (wr) {
#ifdef VAR_PLAYGROUND
        gf.fqn_prefix[LEVELPREFIX] = gf.fqn_prefix[SCOREPREFIX];
        gf.fqn_prefix[SAVEPREFIX] = gf.fqn_prefix[SCOREPREFIX];
        gf.fqn_prefix[BONESPREFIX] = gf.fqn_prefix[SCOREPREFIX];
        gf.fqn_prefix[LOCKPREFIX] = gf.fqn_prefix[SCOREPREFIX];
        gf.fqn_prefix[TROUBLEPREFIX] = gf.fqn_prefix[SCOREPREFIX];
#endif
        check_recordfile(dir);
    }
}
#endif /* CHDIR */

/* returns True iff we set plname[] to username which contains a hyphen */
static boolean
whoami(void)
{
    /*
     * Who am i? Algorithm: 1. Use name as specified in NETHACKOPTIONS
     *                      2. Use $USER or $LOGNAME    (if 1. fails)
     *                      3. Use getlogin()           (if 2. fails)
     * The resulting name is overridden by command line options.
     * If everything fails, or if the resulting name is some generic
     * account like "games", "play", "player", "hack" then eventually
     * we'll ask him.
     * Note that we trust the user here; it is possible to play under
     * somebody else's name.
     */
    if (!*svp.plname) {
        const char *s;

        s = nh_getenv("USER");
        if (!s || !*s)
            s = nh_getenv("LOGNAME");
        if (!s || !*s)
            s = getlogin();

        if (s && *s) {
            (void) strncpy(svp.plname, s, sizeof svp.plname - 1);
            if (strchr(svp.plname, '-'))
                return TRUE;
        }
    }
    return FALSE;
}

#ifndef NO_SIGNAL
void
sethanguphandler(void (*handler)(int))
{
#ifdef SA_RESTART
    /* don't want reads to restart.  If SA_RESTART is defined, we know
     * sigaction exists and can be used to ensure reads won't restart.
     * If it's not defined, assume reads do not restart.  If reads restart
     * and a signal occurs, the game won't do anything until the read
     * succeeds (or the stream returns EOF, which might not happen if
     * reading from, say, a window manager). */
    struct sigaction sact;

    (void) memset((genericptr_t) &sact, 0, sizeof sact);
    sact.sa_handler = (SIG_RET_TYPE) handler;
    (void) sigaction(SIGHUP, &sact, (struct sigaction *) 0);
#ifdef SIGXCPU
    (void) sigaction(SIGXCPU, &sact, (struct sigaction *) 0);
#endif
#else /* !SA_RESTART */
    (void) signal(SIGHUP, (SIG_RET_TYPE) handler);
#ifdef SIGXCPU
    (void) signal(SIGXCPU, (SIG_RET_TYPE) handler);
#endif
#endif /* ?SA_RESTART */
}
#else /* NO_SIGNAL */
/* Stub for WASM builds where signals are not available.
 * Referenced by various parts of the codebase but never functional. */
void
sethanguphandler(void (*handler)(int))
{
    /* no-op: signals not supported in WASM */
    nhUse(handler);
}
#endif /* !NO_SIGNAL */

#ifdef PORT_HELP
void
port_help(void)
{
    /*
     * Display unix-specific help.   Just show contents of the helpfile
     * named by PORT_HELP.
     */
    display_file(PORT_HELP, TRUE);
}
#endif

/* validate wizard mode if player has requested access to it */
boolean
authorize_wizard_mode(void)
{
    if (sysopt.wizards && sysopt.wizards[0]) {
        if (check_user_string(sysopt.wizards))
            return TRUE;
    }
    iflags.wiz_error_flag = TRUE; /* not being allowed into wizard mode */
    return FALSE;
}

/* similar to above, validate explore mode access */
boolean
authorize_explore_mode(void)
{
#ifdef SYSCF
    if (sysopt.explorers && sysopt.explorers[0]) {
        if (check_user_string(sysopt.explorers))
            return TRUE;
    }
    iflags.explore_error_flag = TRUE; /* not allowed into explore mode */
    return FALSE;
#else
    return TRUE; /* if sysconf disabled, no restrictions on explore mode */
#endif
}

static void
wd_message(void)
{
    if (iflags.wiz_error_flag) {
        if (sysopt.wizards && sysopt.wizards[0]) {
            char *tmp = build_english_list(sysopt.wizards);
            pline("Only user%s %s may access debug (wizard) mode.",
                  strchr(sysopt.wizards, ' ') ? "s" : "", tmp);
            free(tmp);
        } else {
            You("cannot access debug (wizard) mode.");
        }
        wizard = FALSE; /* (paranoia) */
        if (!iflags.explore_error_flag)
            pline("Entering explore/discovery mode instead.");
    } else if (iflags.explore_error_flag) {
        You("cannot access explore mode."); /* same as enter_explore_mode */
        discover = iflags.deferred_X = FALSE; /* (more paranoia) */
    } else if (discover)
        You("are in non-scoring explore/discovery mode.");
}

/*
 * Add a slash to any name not ending in /. There must
 * be room for the /
 */
void
append_slash(char *name)
{
    char *ptr;

    if (!*name)
        return;
    ptr = name + (strlen(name) - 1);
    if (*ptr != '/') {
        *++ptr = '/';
        *++ptr = '\0';
    }
    return;
}

boolean
check_user_string(const char *optstr)
{
    struct passwd *pw;
    int pwlen;
    const char *eop, *w;
    char *pwname = 0;

    if (optstr[0] == '*')
        return TRUE; /* allow any user */
    if (sysopt.check_plname)
        pwname = svp.plname;
    else if ((pw = get_unix_pw()) != 0)
        pwname = pw->pw_name;
    if (!pwname || !*pwname)
        return FALSE;
    pwlen = (int) strlen(pwname);
    eop = eos((char *)optstr);
    w = optstr;
    while (w + pwlen <= eop) {
        if (!*w)
            break;
        if (isspace(*w)) {
            w++;
            continue;
        }
        if (!strncmp(w, pwname, pwlen)) {
            if (!w[pwlen] || isspace(w[pwlen]))
                return TRUE;
        }
        while (*w && !isspace(*w))
            w++;
    }
    return FALSE;
}

static struct passwd *
get_unix_pw(void)
{
    char *user;
    unsigned uid;
    static struct passwd *pw = (struct passwd *) 0;

    if (pw)
        return pw; /* cache answer */

    uid = (unsigned) getuid();
    user = getlogin();
    if (user) {
        pw = getpwnam(user);
        if (pw && ((unsigned) pw->pw_uid != uid))
            pw = 0;
    }
    if (pw == 0) {
        user = nh_getenv("USER");
        if (user) {
            pw = getpwnam(user);
            if (pw && ((unsigned) pw->pw_uid != uid))
                pw = 0;
        }
        if (pw == 0) {
            pw = getpwuid(uid);
        }
    }
    return pw;
}

char *
get_login_name(void)
{
    static char buf[BUFSZ];
    struct passwd *pw = get_unix_pw();

    buf[0] = '\0';
    if (pw)
        (void)strcpy(buf, pw->pw_name);

    return buf;
}

unsigned long
sys_random_seed(void)
{
    unsigned long seed = 0L;
    unsigned long pid = (unsigned long) getpid();
    boolean no_seed = TRUE;
#ifdef DEV_RANDOM
    FILE *fptr;

    fptr = fopen(DEV_RANDOM, "r");
    if (fptr) {
        fread(&seed, sizeof (long), 1, fptr);
        has_strong_rngseed = TRUE;  /* decl.c */
        no_seed = FALSE;
        (void) fclose(fptr);
    } else {
        /* leaves clue, doesn't exit */
        paniclog("sys_random_seed", "falling back to weak seed");
    }
#endif
    if (no_seed) {
        seed = (unsigned long) getnow(); /* time((TIME_type) 0) */
        /* Quick dirty band-aid to prevent PRNG prediction */
        if (pid) {
            if (!(pid & 3L))
                pid -= 1L;
            seed *= pid;
        }
    }
    return seed;
}

/* for command-line options that perform some immediate action and then
   terminate the program without starting play, like 'nethack --version'
   or 'nethack -s Zelda'; do some cleanup before that termination */
ATTRNORETURN static void
opt_terminate(void)
{
    config_error_done(); /* free memory allocated by config_error_init() */

    nh_terminate(EXIT_SUCCESS);
    /*NOTREACHED*/
}

/* show the sysconf file name, playground directory, run-time configuration
   file name, dumplog file name if applicable, and some other things */
ATTRNORETURN void
after_opt_showpaths(const char *dir)
{
#ifdef CHDIR
    chdirx(dir, FALSE);
#else
    nhUse(dir);
#endif
    opt_terminate();
    /*NOTREACHED*/
}

#ifdef __EMSCRIPTEN__
/***
 * Helpers
 ***/
EM_JS(void, js_helpers_init, (), {
    globalThis.nethackGlobal = globalThis.nethackGlobal || {};
    globalThis.nethackGlobal.helpers = globalThis.nethackGlobal.helpers || {};

    installHelper(displayInventory, "displayInventory");
    installHelper(getPointerValue, "getPointerValue");
    installHelper(setPointerValue, "setPointerValue");
    installHelper(mapGlyphInfoHelper, "mapGlyphInfoHelper");
    
    function displayInventory() {
        return _repopulate_perminvent();
    }

    function getPointerValue(name, ptr, type) {
        switch(type) {
        case "s": // string
            // var value = UTF8ToString(getValue(ptr, "*"));
            return UTF8ToString(ptr);
        case "p": // pointer
            if(!ptr) return 0; // null pointer
            return getValue(ptr, "*");
        case "c": // char
            return String.fromCharCode(getValue(ptr, "i8"));
        case "b":
            return getValue(ptr, "i8") == 1;
        case "0": /* 2^0 = 1 byte */
            return getValue(ptr, "i8");
        case "1": /* 2^1 = 2 bytes */
            return getValue(ptr, "i16");
        case "2": /* 2^2 = 4 bytes */
        case "i": // integer
        case "n": // number
            return getValue(ptr, "i32");
        case "f": // float
            return getValue(ptr, "float");
        case "d": // double
            return getValue(ptr, "double");
        case "v": // void
            return undefined;
        default:
            throw new TypeError ("unknown type:" + type);
        }
    }

    function setPointerValue(name, ptr, type, value = 0) {
        switch (type) {
        case "p":
            setValue(ptr, value, "*");
            break;
        case "s":
            if(typeof value !== "string")
                throw new TypeError(`expected ${name} return type to be string`);
            stringToUTF8(value, ptr, 256);
            break;
        case "i":
            if(typeof value !== "number" || !Number.isInteger(value))
                throw new TypeError(`expected ${name} return type to be integer`);
            setValue(ptr, value, "i32");
            break;
        case "1":
            if(typeof value !== "number" || !Number.isInteger(value))
                throw new TypeError(`expected ${name} return type to be integer`);
            setValue(ptr, value, "i16");
            break;
        case "c":
            if(typeof value !== "number" || value < 0 || value > 128)
                throw new TypeError(`expected ${name} return type to be integer representing an ASCII character`);
            setValue(ptr, value, "i8");
            break;
        case "f":
        case "d":
            if(typeof value !== "number")
                throw new TypeError(`expected ${name} return type to be number`);
            setValue(ptr, value, "double");
            break;
        case "b":
            if (typeof value !== "boolean")
                throw new TypeError(`expected ${name} return type to be boolean`);
            setValue(ptr, value ? 1 : 0, "i8");
            break;
        case "v":
            break;
        default:
            throw new Error("unknown type");
        }
    }


    function mapGlyphInfoHelper(glyph, x, y, mgflags) {
        /* sizeof(glyph_info) = 36 on WASM32 with ENHANCED_SYMBOLS */
        var ptr = _malloc(36);
        _map_glyphinfo(x, y, glyph, mgflags || 0, ptr);
        var result = {
            glyph:       getValue(ptr +  0, "i32"),
            ttychar:     getValue(ptr +  4, "i32"),
            framecolor:  getValue(ptr +  8, "i32"),
            glyphflags:  getValue(ptr + 12, "i32"),
            color:       getValue(ptr + 16, "i32"),
            symidx:      getValue(ptr + 20, "i32"),
            customcolor: getValue(ptr + 24, "i32"),
            color256idx: getValue(ptr + 28, "i16"),
            tileidx:     getValue(ptr + 30, "i16"),
            x: x, y: y, mgflags: mgflags
        };
        result.ch = String.fromCharCode(result.ttychar & 0xFF);
        _free(ptr);
        return result;
    }

    function installHelper(fn, name) {
        name = name || fn.name;
        globalThis.nethackGlobal.helpers[name] = fn;
    }
})

/***
 * Constants
 ***/
#define SET_CONSTANT(scope, name) set_const(scope, #name, name);
EM_JS(void, set_const, (char *scope_str, char *name_str, int num), {
    let scope = UTF8ToString(scope_str);
    let name = UTF8ToString(name_str);

    globalThis.nethackGlobal.constants[scope] = globalThis.nethackGlobal.constants[scope] || {};
    globalThis.nethackGlobal.constants[scope][name] = num;
    globalThis.nethackGlobal.constants[scope][num] = name;
});
#define SET_CONSTANT_STRING(scope, name) set_const_str(scope, #name, name);
EM_JS(void, set_const_str, (char *scope_str, char *name_str, char *input_str), {
    let scope = UTF8ToString(scope_str);
    let name = UTF8ToString(name_str);
    let str = UTF8ToString(input_str);

    globalThis.nethackGlobal.constants[scope] = globalThis.nethackGlobal.constants[scope] || {};
    globalThis.nethackGlobal.constants[scope][name] = str;
});
#define SET_POINTER(name) set_const_ptr(#name, (void *)&name);
EM_JS(void, set_const_ptr, (char *name_str, void* ptr), {
    let name = UTF8ToString(name_str);

    globalThis.nethackGlobal.pointers = globalThis.nethackGlobal.pointers || {};
    globalThis.nethackGlobal.pointers[name] = ptr;
});

void js_constants_init() {
    EM_ASM({
        globalThis.nethackGlobal = globalThis.nethackGlobal || {};
        globalThis.nethackGlobal.constants = globalThis.nethackGlobal.constants || {};
        globalThis.nethackGlobal.pointers = globalThis.nethackGlobal.pointers || {};
    });

    /* create_nhwindow */
    SET_CONSTANT("WIN_TYPE", NHW_MESSAGE)
    SET_CONSTANT("WIN_TYPE", NHW_STATUS)
    SET_CONSTANT("WIN_TYPE", NHW_MAP)
    SET_CONSTANT("WIN_TYPE", NHW_MENU)
    SET_CONSTANT("WIN_TYPE", NHW_TEXT)

    /* status_update */
    SET_CONSTANT("STATUS_FIELD", BL_CHARACTERISTICS)
    SET_CONSTANT("STATUS_FIELD", BL_RESET)
    SET_CONSTANT("STATUS_FIELD", BL_FLUSH)
    SET_CONSTANT("STATUS_FIELD", BL_TITLE)
    SET_CONSTANT("STATUS_FIELD", BL_STR)
    SET_CONSTANT("STATUS_FIELD", BL_DX)
    SET_CONSTANT("STATUS_FIELD", BL_CO)
    SET_CONSTANT("STATUS_FIELD", BL_IN)
    SET_CONSTANT("STATUS_FIELD", BL_WI)
    SET_CONSTANT("STATUS_FIELD", BL_CH)
    SET_CONSTANT("STATUS_FIELD", BL_ALIGN)
    SET_CONSTANT("STATUS_FIELD", BL_SCORE)
    SET_CONSTANT("STATUS_FIELD", BL_CAP)
    SET_CONSTANT("STATUS_FIELD", BL_GOLD)
    SET_CONSTANT("STATUS_FIELD", BL_ENE)
    SET_CONSTANT("STATUS_FIELD", BL_ENEMAX)
    SET_CONSTANT("STATUS_FIELD", BL_XP)
    SET_CONSTANT("STATUS_FIELD", BL_AC)
    SET_CONSTANT("STATUS_FIELD", BL_HD)
    SET_CONSTANT("STATUS_FIELD", BL_TIME)
    SET_CONSTANT("STATUS_FIELD", BL_HUNGER)
    SET_CONSTANT("STATUS_FIELD", BL_HP)
    SET_CONSTANT("STATUS_FIELD", BL_HPMAX)
    SET_CONSTANT("STATUS_FIELD", BL_LEVELDESC)
    SET_CONSTANT("STATUS_FIELD", BL_EXP)
    SET_CONSTANT("STATUS_FIELD", BL_CONDITION)
    SET_CONSTANT("STATUS_FIELD", MAXBLSTATS)

    /* text attributes */
    SET_CONSTANT("ATTR", ATR_NONE);
    SET_CONSTANT("ATTR", ATR_BOLD);
    SET_CONSTANT("ATTR", ATR_DIM);
    SET_CONSTANT("ATTR", ATR_ULINE);
    SET_CONSTANT("ATTR", ATR_BLINK);
    SET_CONSTANT("ATTR", ATR_INVERSE);
    SET_CONSTANT("ATTR", ATR_URGENT);
    SET_CONSTANT("ATTR", ATR_NOHISTORY);

    /* conditions */
    SET_CONSTANT("CONDITION", BL_MASK_BAREH);
    SET_CONSTANT("CONDITION", BL_MASK_BLIND);
    SET_CONSTANT("CONDITION", BL_MASK_BUSY);
    SET_CONSTANT("CONDITION", BL_MASK_CONF);
    SET_CONSTANT("CONDITION", BL_MASK_DEAF);
    SET_CONSTANT("CONDITION", BL_MASK_ELF_IRON);
    SET_CONSTANT("CONDITION", BL_MASK_FLY);
    SET_CONSTANT("CONDITION", BL_MASK_FOODPOIS);
    SET_CONSTANT("CONDITION", BL_MASK_GLOWHANDS);
    SET_CONSTANT("CONDITION", BL_MASK_GRAB);
    SET_CONSTANT("CONDITION", BL_MASK_HALLU);
    SET_CONSTANT("CONDITION", BL_MASK_HELD);
    SET_CONSTANT("CONDITION", BL_MASK_ICY);
    SET_CONSTANT("CONDITION", BL_MASK_INLAVA);
    SET_CONSTANT("CONDITION", BL_MASK_LEV);
    SET_CONSTANT("CONDITION", BL_MASK_PARLYZ);
    SET_CONSTANT("CONDITION", BL_MASK_RIDE);
    SET_CONSTANT("CONDITION", BL_MASK_SLEEPING);
    SET_CONSTANT("CONDITION", BL_MASK_SLIME);
    SET_CONSTANT("CONDITION", BL_MASK_SLIPPERY);
    SET_CONSTANT("CONDITION", BL_MASK_STONE);
    SET_CONSTANT("CONDITION", BL_MASK_STRNGL);
    SET_CONSTANT("CONDITION", BL_MASK_STUN);
    SET_CONSTANT("CONDITION", BL_MASK_SUBMERGED);
    SET_CONSTANT("CONDITION", BL_MASK_TERMILL);
    SET_CONSTANT("CONDITION", BL_MASK_TETHERED);
    SET_CONSTANT("CONDITION", BL_MASK_TRAPPED);
    SET_CONSTANT("CONDITION", BL_MASK_UNCONSC);
    SET_CONSTANT("CONDITION", BL_MASK_WOUNDEDL);
    SET_CONSTANT("CONDITION", BL_MASK_HOLDING);

    /* menu */
    SET_CONSTANT("MENU_SELECT", PICK_NONE);
    SET_CONSTANT("MENU_SELECT", PICK_ONE);
    SET_CONSTANT("MENU_SELECT", PICK_ANY);

    /* copyright */
    SET_CONSTANT_STRING("COPYRIGHT", COPYRIGHT_BANNER_A);
    SET_CONSTANT_STRING("COPYRIGHT", COPYRIGHT_BANNER_B);
    set_const_str("COPYRIGHT", "COPYRIGHT_BANNER_C", (char*) COPYRIGHT_BANNER_C);
    SET_CONSTANT_STRING("COPYRIGHT", COPYRIGHT_BANNER_D);

    /* glyphs */
    SET_CONSTANT("GLYPH", GLYPH_MON_OFF);
    SET_CONSTANT("GLYPH", GLYPH_PET_OFF);
    SET_CONSTANT("GLYPH", GLYPH_INVIS_OFF);
    SET_CONSTANT("GLYPH", GLYPH_DETECT_OFF);
    SET_CONSTANT("GLYPH", GLYPH_BODY_OFF);
    SET_CONSTANT("GLYPH", GLYPH_RIDDEN_OFF);
    SET_CONSTANT("GLYPH", GLYPH_OBJ_OFF);
    SET_CONSTANT("GLYPH", GLYPH_CMAP_OFF);
    SET_CONSTANT("GLYPH", GLYPH_EXPLODE_OFF);
    SET_CONSTANT("GLYPH", GLYPH_ZAP_OFF);
    SET_CONSTANT("GLYPH", GLYPH_SWALLOW_OFF);
    SET_CONSTANT("GLYPH", GLYPH_WARNING_OFF);
    SET_CONSTANT("GLYPH", GLYPH_STATUE_OFF);
    SET_CONSTANT("GLYPH", GLYPH_UNEXPLORED_OFF);
    SET_CONSTANT("GLYPH", GLYPH_NOTHING_OFF);
    SET_CONSTANT("GLYPH", MAX_GLYPH);
    SET_CONSTANT("GLYPH", NO_GLYPH);
    SET_CONSTANT("GLYPH", GLYPH_INVISIBLE);
    SET_CONSTANT("GLYPH", GLYPH_UNEXPLORED);
    SET_CONSTANT("GLYPH", GLYPH_NOTHING);
    SET_CONSTANT("GLYPH", NUMMONS);

    /* struct permonst layout — allows JS to read mons[] fields via pointer */
    set_const("PERMONST", "SIZEOF", sizeof(struct permonst));
    set_const("PERMONST", "PMNAMES", offsetof(struct permonst, pmnames));
    set_const("PERMONST", "NUM_MGENDERS", NUM_MGENDERS);
    set_const("PERMONST", "MLET", offsetof(struct permonst, mlet));
    set_const("PERMONST", "MLEVEL", offsetof(struct permonst, mlevel));
    set_const("PERMONST", "MMOVE", offsetof(struct permonst, mmove));
    set_const("PERMONST", "AC", offsetof(struct permonst, ac));
    set_const("PERMONST", "MR", offsetof(struct permonst, mr));
    set_const("PERMONST", "MALIGNTYP", offsetof(struct permonst, maligntyp));
    set_const("PERMONST", "GENO", offsetof(struct permonst, geno));
    set_const("PERMONST", "MATTK", offsetof(struct permonst, mattk));
    set_const("PERMONST", "CWT", offsetof(struct permonst, cwt));
    set_const("PERMONST", "CNUTRIT", offsetof(struct permonst, cnutrit));
    set_const("PERMONST", "MSOUND", offsetof(struct permonst, msound));
    set_const("PERMONST", "MSIZE", offsetof(struct permonst, msize));
    set_const("PERMONST", "MRESISTS", offsetof(struct permonst, mresists));
    set_const("PERMONST", "MCONVEYS", offsetof(struct permonst, mconveys));
    set_const("PERMONST", "MFLAGS1", offsetof(struct permonst, mflags1));
    set_const("PERMONST", "MFLAGS2", offsetof(struct permonst, mflags2));
    set_const("PERMONST", "MFLAGS3", offsetof(struct permonst, mflags3));
    set_const("PERMONST", "DIFFICULTY", offsetof(struct permonst, difficulty));
    set_const("PERMONST", "MCOLOR", offsetof(struct permonst, mcolor));

    /* colors */
    SET_CONSTANT("COLORS", CLR_BLACK);
    SET_CONSTANT("COLORS", CLR_RED);
    SET_CONSTANT("COLORS", CLR_GREEN);
    SET_CONSTANT("COLORS", CLR_BROWN);
    SET_CONSTANT("COLORS", CLR_BLUE);
    SET_CONSTANT("COLORS", CLR_MAGENTA);
    SET_CONSTANT("COLORS", CLR_CYAN);
    SET_CONSTANT("COLORS", CLR_GRAY);
    SET_CONSTANT("COLORS", NO_COLOR);
    SET_CONSTANT("COLORS", CLR_ORANGE);
    SET_CONSTANT("COLORS", CLR_BRIGHT_GREEN);
    SET_CONSTANT("COLORS", CLR_YELLOW);
    SET_CONSTANT("COLORS", CLR_BRIGHT_BLUE);
    SET_CONSTANT("COLORS", CLR_BRIGHT_MAGENTA);
    SET_CONSTANT("COLORS", CLR_BRIGHT_CYAN);
    SET_CONSTANT("COLORS", CLR_WHITE);
    SET_CONSTANT("COLORS", CLR_MAX);

    /* color attributes */
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_BOLD);
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_DIM);
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_ITALIC);
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_ULINE);
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_BLINK);
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_INVERSE);
    SET_CONSTANT("COLOR_ATTR", BL_ATTCLR_MAX);

    SET_CONSTANT("BL_MASK", BL_MASK_BAREH);
    SET_CONSTANT("BL_MASK", BL_MASK_BLIND);
    SET_CONSTANT("BL_MASK", BL_MASK_BUSY);
    SET_CONSTANT("BL_MASK", BL_MASK_CONF);
    SET_CONSTANT("BL_MASK", BL_MASK_DEAF);
    SET_CONSTANT("BL_MASK", BL_MASK_ELF_IRON);
    SET_CONSTANT("BL_MASK", BL_MASK_FLY);
    SET_CONSTANT("BL_MASK", BL_MASK_FOODPOIS);
    SET_CONSTANT("BL_MASK", BL_MASK_GLOWHANDS);
    SET_CONSTANT("BL_MASK", BL_MASK_GRAB);
    SET_CONSTANT("BL_MASK", BL_MASK_HALLU);
    SET_CONSTANT("BL_MASK", BL_MASK_HELD);
    SET_CONSTANT("BL_MASK", BL_MASK_ICY);
    SET_CONSTANT("BL_MASK", BL_MASK_INLAVA);
    SET_CONSTANT("BL_MASK", BL_MASK_LEV);
    SET_CONSTANT("BL_MASK", BL_MASK_PARLYZ);
    SET_CONSTANT("BL_MASK", BL_MASK_RIDE);
    SET_CONSTANT("BL_MASK", BL_MASK_SLEEPING);
    SET_CONSTANT("BL_MASK", BL_MASK_SLIME);
    SET_CONSTANT("BL_MASK", BL_MASK_SLIPPERY);
    SET_CONSTANT("BL_MASK", BL_MASK_STONE);
    SET_CONSTANT("BL_MASK", BL_MASK_STRNGL);
    SET_CONSTANT("BL_MASK", BL_MASK_STUN);
    SET_CONSTANT("BL_MASK", BL_MASK_SUBMERGED);
    SET_CONSTANT("BL_MASK", BL_MASK_TERMILL);
    SET_CONSTANT("BL_MASK", BL_MASK_TETHERED);
    SET_CONSTANT("BL_MASK", BL_MASK_TRAPPED);
    SET_CONSTANT("BL_MASK", BL_MASK_UNCONSC);
    SET_CONSTANT("BL_MASK", BL_MASK_WOUNDEDL);
    SET_CONSTANT("BL_MASK", BL_MASK_HOLDING);

    SET_CONSTANT("ROLE_RACEMASK", MH_HUMAN);
    SET_CONSTANT("ROLE_RACEMASK", MH_ELF);
    SET_CONSTANT("ROLE_RACEMASK", MH_DWARF);
    SET_CONSTANT("ROLE_RACEMASK", MH_GNOME);
    SET_CONSTANT("ROLE_RACEMASK", MH_ORC);

    SET_CONSTANT("ROLE_GENDMASK", ROLE_MALE);
    SET_CONSTANT("ROLE_GENDMASK", ROLE_FEMALE);
    SET_CONSTANT("ROLE_GENDMASK", ROLE_NEUTER);

    SET_CONSTANT("ROLE_ALIGNMASK", ROLE_LAWFUL);
    SET_CONSTANT("ROLE_ALIGNMASK", ROLE_NEUTRAL);
    SET_CONSTANT("ROLE_ALIGNMASK", ROLE_CHAOTIC);

    SET_CONSTANT("blconditions", bl_bareh);
    SET_CONSTANT("blconditions", bl_blind);
    SET_CONSTANT("blconditions", bl_busy);
    SET_CONSTANT("blconditions", bl_conf);
    SET_CONSTANT("blconditions", bl_deaf);
    SET_CONSTANT("blconditions", bl_elf_iron);
    SET_CONSTANT("blconditions", bl_fly);
    SET_CONSTANT("blconditions", bl_foodpois);
    SET_CONSTANT("blconditions", bl_glowhands);
    SET_CONSTANT("blconditions", bl_grab);
    SET_CONSTANT("blconditions", bl_hallu);
    SET_CONSTANT("blconditions", bl_held);
    SET_CONSTANT("blconditions", bl_icy);
    SET_CONSTANT("blconditions", bl_inlava);
    SET_CONSTANT("blconditions", bl_lev);
    SET_CONSTANT("blconditions", bl_parlyz);
    SET_CONSTANT("blconditions", bl_ride);
    SET_CONSTANT("blconditions", bl_sleeping);
    SET_CONSTANT("blconditions", bl_slime);
    SET_CONSTANT("blconditions", bl_slippery);
    SET_CONSTANT("blconditions", bl_stone);
    SET_CONSTANT("blconditions", bl_strngl);
    SET_CONSTANT("blconditions", bl_stun);
    SET_CONSTANT("blconditions", bl_submerged);
    SET_CONSTANT("blconditions", bl_termill);
    SET_CONSTANT("blconditions", bl_tethered);
    SET_CONSTANT("blconditions", bl_trapped);
    SET_CONSTANT("blconditions", bl_unconsc);
    SET_CONSTANT("blconditions", bl_woundedl);
    SET_CONSTANT("blconditions", bl_holding);
    SET_CONSTANT("blconditions", CONDITION_COUNT);

    SET_CONSTANT("HL", HL_UNDEF);
    SET_CONSTANT("HL", HL_NONE);
    SET_CONSTANT("HL", HL_BOLD);
    SET_CONSTANT("HL", HL_DIM);
    SET_CONSTANT("HL", HL_ITALIC);
    SET_CONSTANT("HL", HL_ULINE);
    SET_CONSTANT("HL", HL_BLINK);
    SET_CONSTANT("HL", HL_INVERSE);

    SET_CONSTANT("MG", MG_HERO);
    SET_CONSTANT("MG", MG_CORPSE);
    SET_CONSTANT("MG", MG_INVIS);
    SET_CONSTANT("MG", MG_DETECT);
    SET_CONSTANT("MG", MG_PET);
    SET_CONSTANT("MG", MG_RIDDEN);
    SET_CONSTANT("MG", MG_STATUE);
    SET_CONSTANT("MG", MG_OBJPILE);
    SET_CONSTANT("MG", MG_BW_LAVA);
    SET_CONSTANT("MG", MG_BW_ICE);
    SET_CONSTANT("MG", MG_BW_SINK);
    SET_CONSTANT("MG", MG_BW_ENGR);
    SET_CONSTANT("MG", MG_NOTHING);
    SET_CONSTANT("MG", MG_UNEXPL);
    SET_CONSTANT("MG", MG_MALE);
    SET_CONSTANT("MG", MG_FEMALE);

    SET_POINTER(extcmdlist);
    SET_POINTER(conditions);
    SET_POINTER(condtests);

    /* roles/races/genders/alignments */
    SET_POINTER(roles);
    SET_POINTER(races);
    SET_POINTER(genders);
    SET_POINTER(aligns);

    /* monster data */
    SET_POINTER(mons);
    SET_POINTER(def_monsyms);

    /* struct class_sym layout — for mlet → display character lookup */
    set_const("CLASS_SYM", "SIZEOF", sizeof(struct class_sym));
    set_const("CLASS_SYM", "SYM", offsetof(struct class_sym, sym));

    /* inventory data */
    set_const_ptr("gi.invent", (void *)&gi.invent);
    SET_POINTER(objects);
    SET_POINTER(obj_descr);

    /* struct obj layout — for walking the inventory linked list */
    set_const("OBJ", "SIZEOF", sizeof(struct obj));
    set_const("OBJ", "NOBJ", offsetof(struct obj, nobj));
    set_const("OBJ", "OTYP", offsetof(struct obj, otyp));
    set_const("OBJ", "OWT", offsetof(struct obj, owt));
    set_const("OBJ", "QUAN", offsetof(struct obj, quan));
    set_const("OBJ", "SPE", offsetof(struct obj, spe));
    set_const("OBJ", "OCLASS", offsetof(struct obj, oclass));
    set_const("OBJ", "INVLET", offsetof(struct obj, invlet));
    set_const("OBJ", "WHERE", offsetof(struct obj, where));
    set_const("OBJ", "OWORNMASK", offsetof(struct obj, owornmask));
    set_const("OBJ", "CORPSENM", offsetof(struct obj, corpsenm));

    /* struct objclass layout — for looking up object type info */
    set_const("OBJCLASS", "SIZEOF", sizeof(struct objclass));
    set_const("OBJCLASS", "OC_NAME_IDX", offsetof(struct objclass, oc_name_idx));
    set_const("OBJCLASS", "OC_DESCR_IDX", offsetof(struct objclass, oc_descr_idx));
    set_const("OBJCLASS", "OC_CLASS", offsetof(struct objclass, oc_class));
    set_const("OBJCLASS", "OC_WEIGHT", offsetof(struct objclass, oc_weight));
    set_const("OBJCLASS", "OC_COST", offsetof(struct objclass, oc_cost));
    set_const("OBJCLASS", "OC_NUTRITION", offsetof(struct objclass, oc_nutrition));

    /* struct objdescr layout — for object name strings */
    set_const("OBJDESCR", "SIZEOF", sizeof(struct objdescr));
    set_const("OBJDESCR", "OC_NAME", offsetof(struct objdescr, oc_name));
    set_const("OBJDESCR", "OC_DESCR", offsetof(struct objdescr, oc_descr));

    /* terrain type constants from levl[x][y].typ (rm.h) */
    SET_CONSTANT("LEVL_TYP", STONE)
    SET_CONSTANT("LEVL_TYP", VWALL)
    SET_CONSTANT("LEVL_TYP", HWALL)
    SET_CONSTANT("LEVL_TYP", SDOOR)
    SET_CONSTANT("LEVL_TYP", SCORR)
    SET_CONSTANT("LEVL_TYP", DOOR)
    SET_CONSTANT("LEVL_TYP", CORR)
    SET_CONSTANT("LEVL_TYP", ROOM)
    SET_CONSTANT("LEVL_TYP", STAIRS)
    SET_CONSTANT("LEVL_TYP", LADDER)
    SET_CONSTANT("LEVL_TYP", FOUNTAIN)
    SET_CONSTANT("LEVL_TYP", THRONE)
    SET_CONSTANT("LEVL_TYP", SINK)
    SET_CONSTANT("LEVL_TYP", GRAVE)
    SET_CONSTANT("LEVL_TYP", ALTAR)
    SET_CONSTANT("LEVL_TYP", POOL)
    SET_CONSTANT("LEVL_TYP", MOAT)
    SET_CONSTANT("LEVL_TYP", LAVAPOOL)
    SET_CONSTANT("LEVL_TYP", IRONBARS)
    SET_CONSTANT("LEVL_TYP", TREE)
    SET_CONSTANT("LEVL_TYP", ICE)

    /* player property indices (prop.h enum prop_types) */
    SET_CONSTANT("PROP", FIRE_RES)
    SET_CONSTANT("PROP", COLD_RES)
    SET_CONSTANT("PROP", SLEEP_RES)
    SET_CONSTANT("PROP", DISINT_RES)
    SET_CONSTANT("PROP", SHOCK_RES)
    SET_CONSTANT("PROP", POISON_RES)
    SET_CONSTANT("PROP", ACID_RES)
    SET_CONSTANT("PROP", STONE_RES)
    SET_CONSTANT("PROP", DRAIN_RES)
    SET_CONSTANT("PROP", SICK_RES)
    SET_CONSTANT("PROP", INVULNERABLE)
    SET_CONSTANT("PROP", ANTIMAGIC)
    SET_CONSTANT("PROP", STUNNED)
    SET_CONSTANT("PROP", CONFUSION)
    SET_CONSTANT("PROP", BLINDED)
    SET_CONSTANT("PROP", DEAF)
    SET_CONSTANT("PROP", SICK)
    SET_CONSTANT("PROP", STONED)
    SET_CONSTANT("PROP", STRANGLED)
    SET_CONSTANT("PROP", VOMITING)
    SET_CONSTANT("PROP", GLIB)
    SET_CONSTANT("PROP", SLIMED)
    SET_CONSTANT("PROP", HALLUC)
    SET_CONSTANT("PROP", HALLUC_RES)
    SET_CONSTANT("PROP", FUMBLING)
    SET_CONSTANT("PROP", WOUNDED_LEGS)
    SET_CONSTANT("PROP", SLEEPY)
    SET_CONSTANT("PROP", HUNGER)
    SET_CONSTANT("PROP", SEE_INVIS)
    SET_CONSTANT("PROP", TELEPAT)
    SET_CONSTANT("PROP", WARNING)
    SET_CONSTANT("PROP", WARN_OF_MON)
    SET_CONSTANT("PROP", WARN_UNDEAD)
    SET_CONSTANT("PROP", SEARCHING)
    SET_CONSTANT("PROP", CLAIRVOYANT)
    SET_CONSTANT("PROP", INFRAVISION)
    SET_CONSTANT("PROP", DETECT_MONSTERS)
    SET_CONSTANT("PROP", BLND_RES)
    SET_CONSTANT("PROP", ADORNED)
    SET_CONSTANT("PROP", INVIS)
    SET_CONSTANT("PROP", DISPLACED)
    SET_CONSTANT("PROP", STEALTH)
    SET_CONSTANT("PROP", AGGRAVATE_MONSTER)
    SET_CONSTANT("PROP", CONFLICT)
    SET_CONSTANT("PROP", JUMPING)
    SET_CONSTANT("PROP", TELEPORT)
    SET_CONSTANT("PROP", TELEPORT_CONTROL)
    SET_CONSTANT("PROP", LEVITATION)
    SET_CONSTANT("PROP", FLYING)
    SET_CONSTANT("PROP", WWALKING)
    SET_CONSTANT("PROP", SWIMMING)
    SET_CONSTANT("PROP", MAGICAL_BREATHING)
    SET_CONSTANT("PROP", PASSES_WALLS)
    SET_CONSTANT("PROP", SLOW_DIGESTION)
    SET_CONSTANT("PROP", HALF_SPDAM)
    SET_CONSTANT("PROP", HALF_PHDAM)
    SET_CONSTANT("PROP", REGENERATION)
    SET_CONSTANT("PROP", ENERGY_REGENERATION)
    SET_CONSTANT("PROP", PROTECTION)
    SET_CONSTANT("PROP", PROT_FROM_SHAPE_CHANGERS)
    SET_CONSTANT("PROP", POLYMORPH)
    SET_CONSTANT("PROP", POLYMORPH_CONTROL)
    SET_CONSTANT("PROP", UNCHANGING)
    SET_CONSTANT("PROP", FAST)
    SET_CONSTANT("PROP", REFLECTING)
    SET_CONSTANT("PROP", FREE_ACTION)
    SET_CONSTANT("PROP", FIXED_ABIL)
    SET_CONSTANT("PROP", LIFESAVED)

    /* struct prop layout for reading uprops from WASM memory */
    set_const("PROP_STRUCT", "SIZEOF", sizeof(struct prop));
    set_const("PROP_STRUCT", "EXTRINSIC", offsetof(struct prop, extrinsic));
    set_const("PROP_STRUCT", "BLOCKED", offsetof(struct prop, blocked));
    set_const("PROP_STRUCT", "INTRINSIC", offsetof(struct prop, intrinsic));
}

/***
 * Globals
 ***/
#define CREATE_GLOBAL(var, type) create_global(#var, (void *)&var, type);
#define CREATE_GLOBAL_FROM_ARRAY(base, iter, path, end_expr, type) \
    for(iter = 0; end_expr; iter++) { \
        snprintf(buf, BUFSZ, #base ".%d." #path, iter); \
        create_global(buf, (void *)(&(base[iter].path)), type); \
    }

void create_global (char *name, void *ptr, char *type);

void js_globals_init() {
    EM_ASM({
        globalThis.nethackGlobal = globalThis.nethackGlobal || {};
        globalThis.nethackGlobal.globals = globalThis.nethackGlobal.globals || {};
    });

    /* globals */
    CREATE_GLOBAL(svp.plname, "s");
    CREATE_GLOBAL(svp.pl_character, "s");

    /* window globals */
    CREATE_GLOBAL(WIN_MAP, "i");
    CREATE_GLOBAL(WIN_MESSAGE, "i");
    CREATE_GLOBAL(WIN_INVEN, "i");
    CREATE_GLOBAL(WIN_STATUS, "i");

    /* instance flags */
    CREATE_GLOBAL(iflags.window_inited, "b");
    CREATE_GLOBAL(iflags.wc2_hitpointbar, "b");
    CREATE_GLOBAL(iflags.wc_hilite_pet, "b");
    CREATE_GLOBAL(iflags.hilite_pile, "b");

    /* flags */
    CREATE_GLOBAL(flags.initrole, "i");
    CREATE_GLOBAL(flags.initrace, "i");
    CREATE_GLOBAL(flags.initgend, "i");
    CREATE_GLOBAL(flags.initalign, "i");
    CREATE_GLOBAL(flags.showexp, "b");
    CREATE_GLOBAL(flags.time, "b");
}

EM_JS(void, create_global, (char *name_str, void *ptr, char *type_str), {
    let name = UTF8ToString(name_str);
    let type = UTF8ToString(type_str);

    // get helpers
    let getPointerValue = globalThis.nethackGlobal.helpers.getPointerValue;
    let setPointerValue = globalThis.nethackGlobal.helpers.setPointerValue;

    let { obj, prop } = createPath(globalThis.nethackGlobal.globals, name);

    // setters / getters with bound pointers
    Object.defineProperty(obj, prop, {
        get: getPointerValue.bind(null, name, ptr, type),
        set: setPointerValue.bind(null, name, ptr, type),
        configurable: true,
        enumerable: true
    });

    function createPath(obj, path) {
        path = path.split(".");
        let i;
        for (i = 0; i < path.length - 1; i++) {
            // obj[path[i]] = obj[path[i]] || {};
            if (obj[path[i]] === undefined) {
                obj[path[i]] = {};
            }
            obj = obj[path[i]];
        }

        return { obj, prop: path[i] };
    }
})

#endif

/*libnhmain.c*/
