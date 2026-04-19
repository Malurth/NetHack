/* NetHack 3.6  libnhmain.c */
/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/*-Copyright (c) Robert Patrick Rankin, 2011. */
/* NetHack may be freely redistributed.  See license for details. */

/* main.c - WASM/Library NetHack entry point for 3.6
 * Adapted from sys/unix/unixmain.c and NetHack 3.7's sys/libnh/libnhmain.c.
 */

#include "hack.h"
#include "dlb.h"
#include "patchlevel.h"
#include "date.h"
#include "func_tab.h"

#include <ctype.h>
#include <sys/stat.h>
#include <signal.h>
#include <pwd.h>
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
 * 3.6.7 lacks program_state.input_state, so we track this manually.
 * Set to 1 at getpos() entry, cleared at exit. */
int in_getpos = 0;

/* Global click coordinate buffer for nh_poskey position input.
 * See 3.7 libnhmain.c for full explanation of the Asyncify issue. */
int poskey_click_x = 0;
int poskey_click_y = 0;
int poskey_click_mod = 0;

/* Global pick_list buffer for shim_select_menu.
 * Same Asyncify workaround as poskey_click — JS writes the MENU_ITEM_P
 * array pointer here, and shim_select_menu copies it to *menu_list
 * after the callback returns and the stack is restored. */
MENU_ITEM_P *select_menu_pick_list = NULL;

MENU_ITEM_P **
get_select_menu_pick_list_ptr(void)
{
    return &select_menu_pick_list;
}

int *
get_poskey_click_x_ptr()
{
    return &poskey_click_x;
}

int *
get_poskey_click_y_ptr()
{
    return &poskey_click_y;
}

int *
get_poskey_click_mod_ptr()
{
    return &poskey_click_mod;
}

/* Return the current input state.
 * Returns 2 when in position selection mode (farlook, targeting, etc.),
 * 0 otherwise. Matches the 3.7 InputState enum values. */
int
get_input_state()
{
    return in_getpos ? 2 : 0;
}

/* Return the player's actual map coordinates (u.ux, u.uy).
 * Unlike the cursor position, these are always the player's true location
 * even during farlook or targeting. */
int
get_player_x()
{
    return u.ux;
}

int
get_player_y()
{
    return u.uy;
}

/* Return the vision flags at (x,y) from the viz_array.
 * Bit 0 (COULD_SEE=0x1): has line-of-sight if it were lit.
 * Bit 1 (IN_SIGHT=0x2): actually visible (lit + LOS).
 * Bit 2 (TEMP_LIT=0x4): temporarily illuminated.
 * Returns 0 for out-of-bounds. */
int
get_vision_at(x, y)
int x, y;
{
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return 0;
    return (int)viz_array[y][x];
}

/* Return whether the tile at (x,y) is in a lit room.
 * Returns: 1 = lit, 0 = dark, -1 = out-of-bounds or unseen. */
int
get_levl_lit(x, y)
int x, y;
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
get_levl_roomno(x, y)
int x, y;
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
get_levl_typ(x, y)
int x, y;
{
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return -1;
    if (levl[x][y].seenv == 0)
        return -1;
    return (int)levl[x][y].typ;
}

/* Return the display color for the terrain at (x,y).
 * Uses back_to_glyph + mapglyph to get the exact same color
 * that print_glyph would use. Returns -1 if invalid. */
int
get_feature_color(x, y)
int x, y;
{
    int glyph, ch, color;
    unsigned special;
    boolean saved;
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return -1;
    /* Temporarily force use_color so mapglyph returns real colors
     * even if the window port hasn't initialized color mode yet. */
    saved = iflags.use_color;
    iflags.use_color = TRUE;
    glyph = back_to_glyph(x, y);
    mapglyph(glyph, &ch, &color, &special, x, y, 0);
    iflags.use_color = saved;
    return color;
}

/* Fill the provided buffer with rich terrain data for every map tile.
 * Buffer must be at least COLNO * ROWNO * 5 bytes. Layout is row-major,
 * 5 bytes per tile:
 *   [0] char    — display glyph (back_to_glyph + mapglyph)
 *   [1] color   — NetHack color enum (0-15)
 *   [2] typ     — terrain enum from levl[x][y].typ
 *   [3] vision  — vision flags from viz_array[y][x] (COULD_SEE|IN_SIGHT|TEMP_LIT)
 *   [4] flags   — bit 0 = lit, bits 1-7 = roomno (0-63)
 *
 * Index of tile (x, y) is `(y * COLNO + x) * 5`. */
void
get_terrain_map(out_buffer)
unsigned char *out_buffer;
{
    int g, ch, color, x, y, idx;
    unsigned special;
    unsigned char vision;
    boolean saved;

    if (!out_buffer)
        return;

    saved = iflags.use_color;
    iflags.use_color = TRUE;

    for (y = 0; y < ROWNO; y++) {
        for (x = 0; x < COLNO; x++) {
            idx = (y * COLNO + x) * 5;
            vision = (unsigned char)(viz_array[y][x] & 0xFF);
            if (levl[x][y].seenv == 0) {
                out_buffer[idx]     = ' ';
                out_buffer[idx + 1] = 0;
                out_buffer[idx + 2] = 0;
                out_buffer[idx + 3] = vision;
                out_buffer[idx + 4] = 0;
            } else {
                g = back_to_glyph(x, y);
                mapglyph(g, &ch, &color, &special, x, y, 0);
                out_buffer[idx]     = (unsigned char)(ch & 0xFF);
                out_buffer[idx + 1] = (unsigned char)(color & 0xFF);
                out_buffer[idx + 2] = (unsigned char)(levl[x][y].typ & 0xFF);
                out_buffer[idx + 3] = vision;
                out_buffer[idx + 4] = (unsigned char)(
                    (levl[x][y].lit ? 1 : 0) |
                    ((levl[x][y].roomno & 0x7F) << 1)
                );
            }
        }
    }

    iflags.use_color = saved;
}

/* Return the clean screen description for position (x,y).
 * Calls do_screen_description() and returns the firstmatch string —
 * the same unambiguous description that auto_describe() displays.
 * Returns empty string for invalid coordinates or no description. */
const char *
get_screen_description(x, y)
int x, y;
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
get_terrain_description(x, y)
int x, y;
{
    static char result_buf[BUFSZ];
    char tmpbuf[BUFSZ];
    const char *desc;

    result_buf[0] = '\0';
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return result_buf;

    desc = dfeature_at(x, y, tmpbuf);
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
get_monster_givenname(x, y)
int x, y;
{
    struct monst *mon;
    if (x < 0 || x >= COLNO || y < 0 || y >= ROWNO)
        return "";
    mon = m_at(x, y);
    if (mon && has_mname(mon))
        return MNAME(mon);
    return "";
}

/* Look up an extended command by name, returning its index in extcmdlist.
 * Returns -1 if not found. */
int
get_extcmd_index(name)
const char *name;
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
 * 3.6.7 uses separate globals for up/down stairs and ladders. */
int
get_stair_direction(x, y)
int x, y;
{
    if (x == xupstair && y == yupstair)
        return 1;
    if (x == xdnstair && y == ydnstair)
        return 2;
    if (x == xupladder && y == yupladder)
        return 3;
    if (x == xdnladder && y == ydnladder)
        return 4;
    /* sstairs (special stairs, e.g. quest portal) */
    if (x == sstairs.sx && y == sstairs.sy)
        return sstairs.up ? 1 : 2;
    return 0;
}

/* Return pointer to player's properties array u.uprops[].
 * Each element is a struct prop { long extrinsic, blocked, intrinsic }.
 * Frontends walk the array using PROP indices from js_constants_init. */
struct prop *
get_uprops_ptr()
{
    return u.uprops;
}

/* Return the number of properties (LAST_PROP + 1). */
int
get_uprops_count()
{
    return LAST_PROP + 1;
}

/* Return a comma-separated string of monster types/species that the
 * player is currently warned about via WARN_OF_MON.  Decodes M2 flags
 * from context.warntype.obj (artifact sources) and .polyd (polymorph
 * sources), plus the specific species (if any).  Returns "" when no
 * warn-of-mon sources are active. */
const char *
get_warntype_text()
{
    static char buf[BUFSZ];
    buf[0] = '\0';

    unsigned long combined = context.warntype.obj
                           | context.warntype.polyd;

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
    if (context.warntype.speciesidx >= LOW_PM
        && context.warntype.speciesidx < NUMMONS) {
        if (*buf) Strcat(buf, ",");
        Strcat(buf, mons[context.warntype.speciesidx].mname);
    }

    return buf;
}

#if !defined(_BULL_SOURCE) && !defined(__sgi) && !defined(_M_UNIX)
#if !defined(SUNOS4) && !(defined(ULTRIX) && defined(__GNUC__))
#if defined(POSIX_TYPES) || defined(SVR4) || defined(HPUX)
extern struct passwd *getpwuid(uid_t);
#else
extern struct passwd *getpwuid(int);
#endif
#endif
#endif
extern struct passwd *getpwnam(const char *);
#ifdef CHDIR
static void chdirx(const char *, boolean);
#endif /* CHDIR */
static boolean whoami(void);
static void process_options(int, char **);

static void wd_message(void);
static boolean wiz_error_flag = FALSE;
static struct passwd *get_unix_pw(void);

#ifdef __EMSCRIPTEN__
/* if WebAssembly, export this API and don't optimize it out */
EMSCRIPTEN_KEEPALIVE
#endif
int
main(int argc, char *argv[])
{
    register int fd;
#ifdef CHDIR
    register char *dir;
#endif
    boolean exact_username;
    boolean resuming = FALSE; /* assume new game */
    boolean plsel_once = FALSE;

    sys_early_init();

    hname = argv[0];
    hackpid = getpid();
    (void) umask(0777 & ~FCMASK);

    choose_windows(DEFAULT_WINDOW_SYS);

#ifdef CHDIR /* otherwise no chdir() */
    dir = nh_getenv("NETHACKDIR");
    if (!dir)
        dir = nh_getenv("HACKDIR");

    if (argc > 1) {
        if (argcheck(argc, argv, ARG_VERSION) == 2)
            exit(EXIT_SUCCESS);

        if (argcheck(argc, argv, ARG_SHOWPATHS) == 2) {
#ifdef CHDIR
            chdirx((char *) 0, 0);
#endif
            iflags.initoptions_noterminate = TRUE;
            initoptions();
            iflags.initoptions_noterminate = FALSE;
            reveal_paths(EXIT_SUCCESS);
            exit(EXIT_SUCCESS);
        }
        if (argcheck(argc, argv, ARG_DEBUG) == 1) {
            argc--;
            argv++;
        }
        if (argc > 1 && !strncmp(argv[1], "-d", 2) && argv[1][2] != 'e') {
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
        if (!strncmp(argv[1], "-s", 2) && strncmp(argv[1], "-style", 6)) {
#ifdef CHDIR
            chdirx(dir, 0);
#endif
#ifdef SYSCF
            initoptions();
#endif
#ifdef PANICTRACE
            ARGV0 = hname;
#ifndef NO_SIGNAL
            panictrace_setsignals(TRUE);
#endif
#endif
            prscore(argc, argv);
            exit(EXIT_SUCCESS);
        }
    }

#ifdef CHDIR
    chdirx(dir, 1);
#endif

#ifdef __EMSCRIPTEN__
    js_helpers_init();
    js_constants_init();
    js_globals_init();
#endif

    initoptions();
#ifdef PANICTRACE
    ARGV0 = hname;
#ifndef NO_SIGNAL
    panictrace_setsignals(TRUE);
#endif
#endif
    exact_username = whoami();

    u.uhp = 1; /* prevent RIP on early quits */
    program_state.preserve_locks = 1;
#ifndef NO_SIGNAL
    sethanguphandler((SIG_RET_TYPE) hangup);
#endif

    process_options(argc, argv);
#ifdef WINCHAIN
    commit_windowchain();
#endif
    init_nhwindows(&argc, argv);

#ifdef DEF_PAGER
    if (!(catmore = nh_getenv("HACKPAGER"))
        && !(catmore = nh_getenv("PAGER")))
        catmore = DEF_PAGER;
#endif
#ifdef MAIL
    getmailstatus();
#endif

    /* wizard mode access is deferred until here */
    set_playmode(); /* sets plname to "wizard" for wizard mode */
    if (exact_username) {
        int len = (int) strlen(plname);
        if (++len < (int) sizeof plname)
            (void) strncat(strcat(plname, "-"), pl_character,
                           sizeof plname - len - 1);
    }
    plnamesuffix();

    if (wizard) {
        locknum = 0;
    } else {
#ifndef NO_SIGNAL
        (void) signal(SIGQUIT, SIG_IGN);
        (void) signal(SIGINT, SIG_IGN);
#endif
    }

    dlb_init(); /* must be before newgame() */

    vision_init();

    display_gamewindows();

 attempt_restore:

    if (*plname) {
        getlock();
        program_state.preserve_locks = 0;
    }

    if (*plname && (fd = restore_saved_game()) >= 0) {
        const char *fq_save = fqname(SAVEF, SAVEPREFIX, 1);

        (void) chmod(fq_save, 0);
#ifndef NO_SIGNAL
        (void) signal(SIGINT, (SIG_RET_TYPE) done1);
#endif
#ifdef NEWS
        if (iflags.news) {
            display_file(NEWS, FALSE);
            iflags.news = FALSE;
        }
#endif
        pline("Restoring save file...");
        mark_synch();
        if (dorecover(fd)) {
            resuming = TRUE;
            wd_message();
            if (discover || wizard) {
                if (yn("Do you want to keep the save file?") == 'n') {
                    (void) delete_savefile();
                } else {
                    (void) chmod(fq_save, FCMASK);
                    nh_compress(fq_save);
                }
            }
        }
    }

    if (!resuming) {
        boolean neednewlock = (!*plname);
        if (!iflags.renameinprogress || iflags.defer_plname || neednewlock) {
            if (!plsel_once)
                player_selection();
            plsel_once = TRUE;
            if (neednewlock && *plname)
                goto attempt_restore;
            if (iflags.renameinprogress) {
                if (!locknum) {
                    delete_levelfile(0);
                    getlock();
                }
                goto attempt_restore;
            }
        }
        newgame();
        wd_message();
    }

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

    while (argc > 1 && argv[1][0] == '-') {
        argv++;
        argc--;
        l = (int) strlen(*argv);
        if (l < 4)
            l = 4;

        switch (argv[0][1]) {
        case 'D':
        case 'd':
            if ((argv[0][1] == 'D' && !argv[0][2])
                || !strcmpi(*argv, "-debug")) {
                wizard = TRUE, discover = FALSE;
            } else if (!strncmpi(*argv, "-DECgraphics", l)) {
                load_symset("DECGraphics", PRIMARY);
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
                (void) strncpy(plname, argv[0] + 2, sizeof plname - 1);
            } else if (argc > 1) {
                argc--;
                argv++;
                (void) strncpy(plname, argv[0], sizeof plname - 1);
            } else {
                raw_print("Player name expected after -u");
            }
            break;
        case 'I':
        case 'i':
            if (!strncmpi(*argv, "-IBMgraphics", l)) {
                load_symset("IBMGraphics", PRIMARY);
                load_symset("RogueIBM", ROGUESET);
                switch_symbols(TRUE);
            } else {
                raw_printf("Unknown option: %.60s", *argv);
            }
            break;
        case 'p':
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
        case 'r':
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
        case 'w':
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
        }
    }

#ifdef SYSCF
    if (argc > 1)
        raw_printf("MAXPLAYERS are set in sysconf file.\n");
#else
    if (argc > 1)
        locknum = atoi(argv[1]);
#endif
#ifdef MAX_NR_OF_PLAYERS
    if (!locknum || locknum > MAX_NR_OF_PLAYERS)
        locknum = MAX_NR_OF_PLAYERS;
#endif
#ifdef SYSCF
    if (!locknum || (sysopt.maxplayers && locknum > sysopt.maxplayers))
        locknum = sysopt.maxplayers;
#endif
}

#ifdef CHDIR
static void
chdirx(const char *dir, boolean wr)
{
    if (dir
#ifdef HACKDIR
        && strcmp(dir, HACKDIR)
#endif
        ) {
#ifdef SECURE
        (void) setgid(getgid());
        (void) setuid(getuid());
#endif
    } else {
#ifdef VAR_PLAYGROUND
        int len = strlen(VAR_PLAYGROUND);

        fqn_prefix[SCOREPREFIX] = (char *) alloc(len + 2);
        Strcpy(fqn_prefix[SCOREPREFIX], VAR_PLAYGROUND);
        if (fqn_prefix[SCOREPREFIX][len - 1] != '/') {
            fqn_prefix[SCOREPREFIX][len] = '/';
            fqn_prefix[SCOREPREFIX][len + 1] = '\0';
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

    if (wr) {
#ifdef VAR_PLAYGROUND
        fqn_prefix[LEVELPREFIX] = fqn_prefix[SCOREPREFIX];
        fqn_prefix[SAVEPREFIX] = fqn_prefix[SCOREPREFIX];
        fqn_prefix[BONESPREFIX] = fqn_prefix[SCOREPREFIX];
        fqn_prefix[LOCKPREFIX] = fqn_prefix[SCOREPREFIX];
        fqn_prefix[TROUBLEPREFIX] = fqn_prefix[SCOREPREFIX];
#endif
        check_recordfile(dir);
    }
}
#endif /* CHDIR */

void
after_opt_showpaths(dir)
const char *dir UNUSED;
{
#ifdef CHDIR
    chdirx((char *) 0, 0);
#else
    nhUse(dir);
#endif
    nh_terminate(EXIT_SUCCESS);
}

static boolean
whoami(void)
{
    if (!*plname) {
        register const char *s;

        s = nh_getenv("USER");
        if (!s || !*s)
            s = nh_getenv("LOGNAME");
        if (!s || !*s)
            s = getlogin();

        if (s && *s) {
            (void) strncpy(plname, s, sizeof plname - 1);
            if (index(plname, '-'))
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
    display_file(PORT_HELP, TRUE);
}
#endif

boolean
authorize_wizard_mode(void)
{
    if (sysopt.wizards && sysopt.wizards[0] && check_user_string(sysopt.wizards))
        return TRUE;
    wiz_error_flag = TRUE;
    return FALSE;
}

static void
wd_message(void)
{
    if (wiz_error_flag) {
        if (sysopt.wizards && sysopt.wizards[0]) {
            char *tmp = build_english_list(sysopt.wizards);
            pline("Only user%s %s may access debug (wizard) mode.",
                  index(sysopt.wizards, ' ') ? "s" : "", tmp);
            free(tmp);
        } else
            pline("Entering explore/discovery mode instead.");
        wizard = 0, discover = 1;
    } else if (discover)
        You("are in non-scoring explore/discovery mode.");
}

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
check_user_string(char *optstr)
{
    struct passwd *pw;
    int pwlen;
    char *eop, *w;
    char *pwname = 0;

    if (optstr[0] == '*')
        return TRUE;
    if (sysopt.check_plname)
        pwname = plname;
    else if ((pw = get_unix_pw()) != 0)
        pwname = pw->pw_name;
    if (!pwname || !*pwname)
        return FALSE;
    pwlen = (int) strlen(pwname);
    eop = eos(optstr);
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
        return pw;

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
        has_strong_rngseed = TRUE;
        no_seed = FALSE;
        (void) fclose(fptr);
    } else {
        paniclog("sys_random_seed", "falling back to weak seed");
    }
#endif
    if (no_seed) {
        seed = (unsigned long) getnow();
        if (pid) {
            if (!(pid & 3L))
                pid -= 1L;
            seed *= pid;
        }
    }
    return seed;
}


#ifdef __EMSCRIPTEN__

#ifdef USE_TILES
extern short glyph2tile[];

int
glyph_to_tile(glyph)
int glyph;
{
    if (glyph < 0 || glyph >= MAX_GLYPH)
        return -1;
    return (int)glyph2tile[glyph];
}
#endif /* USE_TILES */

/***
 * Helpers
 ***/
EM_JS(void, js_helpers_init, (), {
    globalThis.nethackGlobal = globalThis.nethackGlobal || {};
    globalThis.nethackGlobal.helpers = globalThis.nethackGlobal.helpers || {};

    installHelper(getPointerValue, "getPointerValue");
    installHelper(setPointerValue, "setPointerValue");
    installHelper(mapglyphHelper, "mapglyphHelper");
    installHelper(tileIndexForGlyph, "tileIndexForGlyph");

    function mapglyphHelper(glyph, x, y, mgflags) {
        let ochar = _malloc(4);
        let ocolor = _malloc(4);
        let ospecial = _malloc(4);
        _mapglyph(glyph, ochar, ocolor, ospecial, x, y, mgflags);
        let ch = getValue(ochar, "i32");
        let color = getValue(ocolor, "i32");
        let special = getValue(ospecial, "i32");
        _free(ochar);
        _free(ocolor);
        _free(ospecial);
        return { glyph, ch, color, special, tileIdx: _glyph_to_tile(glyph), x, y, mgflags };
    }

    function tileIndexForGlyph(glyph) {
        return _glyph_to_tile(glyph);
    }

    // convert 'ptr' to the type indicated by 'type'
    function getPointerValue(name, ptr, type) {
        switch(type) {
        case "s": // string
            return UTF8ToString(ptr);
        case "p": // pointer
            if(!ptr) return 0;
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

    // sets the return value of the function to the type expected
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

    /* conditions (3.6 has 13 conditions) */
    SET_CONSTANT("CONDITION", BL_MASK_STONE);
    SET_CONSTANT("CONDITION", BL_MASK_SLIME);
    SET_CONSTANT("CONDITION", BL_MASK_STRNGL);
    SET_CONSTANT("CONDITION", BL_MASK_FOODPOIS);
    SET_CONSTANT("CONDITION", BL_MASK_TERMILL);
    SET_CONSTANT("CONDITION", BL_MASK_BLIND);
    SET_CONSTANT("CONDITION", BL_MASK_DEAF);
    SET_CONSTANT("CONDITION", BL_MASK_STUN);
    SET_CONSTANT("CONDITION", BL_MASK_CONF);
    SET_CONSTANT("CONDITION", BL_MASK_HALLU);
    SET_CONSTANT("CONDITION", BL_MASK_LEV);
    SET_CONSTANT("CONDITION", BL_MASK_FLY);
    SET_CONSTANT("CONDITION", BL_MASK_RIDE);

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
    SET_CONSTANT("GLYPH", MAX_GLYPH);
    SET_CONSTANT("GLYPH", NO_GLYPH);
    SET_CONSTANT("GLYPH", GLYPH_INVISIBLE);
    SET_CONSTANT("GLYPH", NUMMONS);

    /* struct permonst layout — allows JS to read mons[] fields via pointer */
    set_const("PERMONST", "SIZEOF", sizeof(struct permonst));
    set_const("PERMONST", "MNAME", offsetof(struct permonst, mname));
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
#ifdef TEXTCOLOR
    set_const("PERMONST", "MCOLOR", offsetof(struct permonst, mcolor));
#endif

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
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_DIM);
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_BLINK);
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_ULINE);
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_INVERSE);
    SET_CONSTANT("COLOR_ATTR", HL_ATTCLR_BOLD);
    SET_CONSTANT("COLOR_ATTR", BL_ATTCLR_MAX);

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

    SET_CONSTANT("HL", HL_UNDEF);
    SET_CONSTANT("HL", HL_NONE);
    SET_CONSTANT("HL", HL_BOLD);
    SET_CONSTANT("HL", HL_DIM);
    SET_CONSTANT("HL", HL_ULINE);
    SET_CONSTANT("HL", HL_BLINK);
    SET_CONSTANT("HL", HL_INVERSE);

    /* monster glyph flags (3.6 subset) */
    SET_CONSTANT("MG", MG_CORPSE);
    SET_CONSTANT("MG", MG_INVIS);
    SET_CONSTANT("MG", MG_DETECT);
    SET_CONSTANT("MG", MG_PET);
    SET_CONSTANT("MG", MG_RIDDEN);
    SET_CONSTANT("MG", MG_STATUE);
    SET_CONSTANT("MG", MG_OBJPILE);
    SET_CONSTANT("MG", MG_BW_LAVA);

    SET_POINTER(extcmdlist);

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
    set_const_ptr("invent", (void *)&invent);
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

void create_global (char *name, void *ptr, char *type);

void js_globals_init() {
    EM_ASM({
        globalThis.nethackGlobal = globalThis.nethackGlobal || {};
        globalThis.nethackGlobal.globals = globalThis.nethackGlobal.globals || {};
    });

    /* globals — 3.6 uses direct globals (no svp./gh. prefix) */
    CREATE_GLOBAL(plname, "s");
    CREATE_GLOBAL(pl_character, "s");

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

    let getPointerValue = globalThis.nethackGlobal.helpers.getPointerValue;
    let setPointerValue = globalThis.nethackGlobal.helpers.setPointerValue;

    let { obj, prop } = createPath(globalThis.nethackGlobal.globals, name);

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
            if (obj[path[i]] === undefined) {
                obj[path[i]] = {};
            }
            obj = obj[path[i]];
        }

        return { obj, prop: path[i] };
    }
})

#endif /* __EMSCRIPTEN__ */

/*libnhmain.c*/
