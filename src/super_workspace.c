/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * super_workspace.c: Super workspaces: independent sets of workspaces of which
 *                    only one is active at a time. The workspaces of inactive
 *                    super workspaces are stashed on the __i3 pseudo-output.
 *
 */
#include "all.h"
#include "yajl_utils.h"

int current_super_workspace = 0;
bool super_workspace_notifications_all = true;

static int previous_super_workspace = -1;

/* Runtime state of a super workspace, kept across config reloads. */
struct ss_state {
    int num;
    /* The name it was last switched to with (if it is not configured). */
    char *name;
    /* The workspace that was focused when leaving it. */
    char *last_workspace;
    /* previous_workspace_name when leaving it. */
    char *previous_workspace;

    TAILQ_ENTRY(ss_state) states;
};

static TAILQ_HEAD(ss_states_head, ss_state) ss_states = TAILQ_HEAD_INITIALIZER(ss_states);

static struct ss_state *get_state(int num) {
    struct ss_state *state;
    TAILQ_FOREACH (state, &ss_states, states) {
        if (state->num == num)
            return state;
    }
    state = scalloc(1, sizeof(struct ss_state));
    state->num = num;
    TAILQ_INSERT_TAIL(&ss_states, state, states);
    return state;
}

static void replace_string(char **dest, const char *src) {
    FREE(*dest);
    if (src != NULL)
        *dest = sstrdup(src);
}

static struct Super_Workspace_Assignment *get_assignment(int num) {
    struct Super_Workspace_Assignment *assignment;
    TAILQ_FOREACH (assignment, &super_workspace_assignments, super_workspace_assignments) {
        if (assignment->num == num)
            return assignment;
    }
    return NULL;
}

char *super_workspace_name(int num) {
    struct Super_Workspace_Assignment *assignment = get_assignment(num);
    if (assignment != NULL)
        return sstrdup(assignment->name);

    struct ss_state *state = get_state(num);
    if (state->name != NULL)
        return sstrdup(state->name);

    char *name;
    sasprintf(&name, "%d", num);
    return name;
}

const char *super_workspace_user(int num) {
    struct Super_Workspace_Assignment *assignment = get_assignment(num);
    return (assignment != NULL ? assignment->user : NULL);
}

bool con_is_alien(Con *con) {
    if (con->window == NULL || !con->window->uid_known)
        return false;
    Con *ws = con_get_workspace(con);
    if (ws == NULL || con_is_internal(ws))
        return false;

    /* Super workspaces without a (known) user belong to the i3 user. */
    struct Super_Workspace_Assignment *assignment = get_assignment(ws->super_workspace);
    const uid_t expected = (assignment != NULL && assignment->has_uid) ? assignment->uid : getuid();
    return con->window->uid != expected;
}

/* A pinned container and where it was, to be moved into the super workspace
 * which is switched to. */
struct pinned {
    Con *con;
    char *workspace;
    int num;
    char *output;
};

static int collect_pinned(struct pinned **pinned) {
    int count = 0;
    *pinned = NULL;
    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        if (!con->ss_pinned || con->type != CT_CON)
            continue;
        Con *ws = con_get_workspace(con);
        if (ws == NULL || con_is_internal(ws))
            continue;
        *pinned = srealloc(*pinned, (count + 1) * sizeof(struct pinned));
        (*pinned)[count++] = (struct pinned){
            .con = con,
            .workspace = sstrdup(ws->name),
            .num = ws->num,
            .output = sstrdup(con_get_output(ws)->name),
        };
    }
    return count;
}

/* Returns the workspace of the active super workspace with the number (or,
 * for named workspaces, the name) of |pin|'s workspace, creating it on the
 * output the pinned container was on. */
static Con *pinned_target(struct pinned *pin) {
    Con *ws = (pin->num != -1) ? get_existing_workspace_by_num(pin->num)
                               : get_existing_workspace_by_name(pin->workspace);
    if (ws != NULL)
        return ws;

    Output *output = get_output_by_name(pin->output, true);
    if (output == NULL)
        output = get_first_output();
    ws = con_new(NULL, NULL);
    ws->type = CT_WORKSPACE;
    ws->name = sstrdup(pin->workspace);
    ws->num = pin->num;
    ws->super_workspace = current_super_workspace;
    ws->workspace_layout = config.default_layout;
    if (config.default_orientation == NO_ORIENTATION) {
        ws->layout = (output->con->rect.height > output->con->rect.width) ? L_SPLITV : L_SPLITH;
    } else {
        ws->layout = (config.default_orientation == HORIZ) ? L_SPLITH : L_SPLITV;
    }
    con_attach(ws, output_get_content(output->con), false);

    char *name;
    sasprintf(&name, "[i3 con] workspace %s", ws->name);
    x_set_name(ws, name);
    free(name);

    ipc_send_workspace_event("init", ws, NULL);
    return ws;
}

static void move_pinned(struct pinned *pinned, int count) {
    for (int i = 0; i < count; i++) {
        Con *target = pinned_target(&pinned[i]);
        DLOG("Pinned container %p follows to workspace %s\n", pinned[i].con, target->name);
        con_move_to_workspace(pinned[i].con, target, true, true, true);
        free(pinned[i].workspace);
        free(pinned[i].output);
    }
    free(pinned);
}

/* Returns the content container of the __i3 pseudo-output, where the
 * workspaces of inactive super workspaces are stashed. */
static Con *stash_content(void) {
    Con *output;
    TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
        if (strcmp(output->name, "__i3") == 0)
            return output_get_content(output);
    }
    return NULL;
}

static bool workspace_is_empty(Con *ws) {
    return TAILQ_EMPTY(&(ws->nodes_head)) && TAILQ_EMPTY(&(ws->floating_head));
}

static void stash_workspaces(Con *stash) {
    Con *output;
    TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
        if (con_is_internal(output))
            continue;

        Con *content = output_get_content(output);
        Con *ws = TAILQ_FIRST(&(content->nodes_head));
        while (ws != NULL) {
            Con *next = TAILQ_NEXT(ws, nodes);
            if (!con_is_internal(ws)) {
                DLOG("Stashing workspace %p / %s of super workspace %d\n",
                     ws, ws->name, ws->super_workspace);
                ws->ss_output = sstrdup(output->name);
                ws->ss_visible = (ws->fullscreen_mode == CF_OUTPUT);
                ws->fullscreen_mode = CF_NONE;
                con_detach(ws);
                con_attach(ws, stash, false);
            }
            ws = next;
        }
    }
}

static void restore_workspaces(Con *stash, int num) {
    Con *ws = TAILQ_FIRST(&(stash->nodes_head));
    while (ws != NULL) {
        Con *next = TAILQ_NEXT(ws, nodes);
        if (ws->type == CT_WORKSPACE && ws->ss_output != NULL && ws->super_workspace == num) {
            Output *target = get_output_by_name(ws->ss_output, true);
            if (target == NULL)
                target = get_first_output();
            DLOG("Restoring workspace %p / %s to output %s\n", ws, ws->name, output_primary_name(target));
            FREE(ws->ss_output);
            con_detach(ws);
            con_attach(ws, output_get_content(target->con), false);
            if (ws->ss_visible) {
                Con *other;
                TAILQ_FOREACH (other, &(ws->parent->nodes_head), nodes) {
                    other->fullscreen_mode = CF_NONE;
                }
                ws->fullscreen_mode = CF_OUTPUT;
            }
            ws->ss_visible = false;
        }
        ws = next;
    }
}

/* Makes sure every output shows exactly one workspace, creating one if an
 * output has none, and closes restored workspaces which are empty and not
 * visible. */
static void fix_outputs(void) {
    Con *output;
    TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
        if (con_is_internal(output))
            continue;

        Con *content = output_get_content(output);
        if (TAILQ_EMPTY(&(content->nodes_head))) {
            create_workspace_on_output(get_output_for_con(output), content);
            continue;
        }

        if (con_get_fullscreen_con(content, CF_OUTPUT) == NULL)
            TAILQ_FIRST(&(content->nodes_head))->fullscreen_mode = CF_OUTPUT;

        Con *ws = TAILQ_FIRST(&(content->nodes_head));
        while (ws != NULL) {
            Con *next = TAILQ_NEXT(ws, nodes);
            if (ws->fullscreen_mode != CF_OUTPUT && workspace_is_empty(ws)) {
                DLOG("Closing empty workspace %p / %s\n", ws, ws->name);
                yajl_gen gen = ipc_marshal_workspace_event("empty", ws, NULL);
                tree_close_internal(ws, DONT_KILL_WINDOW, false);

                const unsigned char *payload;
                ylength length;
                y(get_buf, &payload, &length);
                ipc_send_event("workspace", I3_IPC_EVENT_WORKSPACE, (const char *)payload);
                y(free);
            }
            ws = next;
        }
    }
}

void super_workspace_switch(int num) {
    if (num == current_super_workspace) {
        DLOG("Already on super workspace %d\n", num);
        return;
    }

    Con *stash = stash_content();
    if (stash == NULL) {
        ELOG("No __i3 pseudo-output, cannot switch super workspaces\n");
        return;
    }

    struct ss_state *old_state = get_state(current_super_workspace);
    Con *focused_ws = con_get_workspace(focused);
    if (focused_ws != NULL && !con_is_internal(focused_ws))
        replace_string(&(old_state->last_workspace), focused_ws->name);
    replace_string(&(old_state->previous_workspace), previous_workspace_name);
    char *focused_output = sstrdup(con_get_output(focused)->name);

    LOG("Switching from super workspace %d to %d\n", current_super_workspace, num);
    struct pinned *pinned;
    const int pinned_count = collect_pinned(&pinned);
    stash_workspaces(stash);

    const int old = current_super_workspace;
    current_super_workspace = num;
    restore_workspaces(stash, num);
    fix_outputs();
    move_pinned(pinned, pinned_count);

    /* Focus the workspace that was focused when leaving the target, or the
     * visible one on the output which had focus. */
    struct ss_state *state = get_state(num);
    Con *target = NULL;
    if (state->last_workspace != NULL)
        target = get_existing_workspace_by_name(state->last_workspace);
    if (target == NULL) {
        Output *output = get_output_by_name(focused_output, true);
        if (output == NULL)
            output = get_first_output();
        target = con_get_fullscreen_con(output_get_content(output->con), CF_OUTPUT);
    }
    free(focused_output);
    workspace_show(target);

    replace_string(&previous_workspace_name, state->previous_workspace);
    previous_super_workspace = old;

    ewmh_update_desktop_properties();
    ipc_send_super_workspace_event("focus", old);
}

void super_workspace_reveal(Con *con) {
    Con *ws = con_get_workspace(con);
    if (ws != NULL && ws->type == CT_WORKSPACE && ws->ss_output != NULL)
        super_workspace_switch(ws->super_workspace);
}

Con *super_workspace_get_workspace(int num, Con *con) {
    Con *stash = stash_content();
    Con *output = con_get_output(con);
    struct ss_state *state = get_state(num);
    Con *fallback = NULL, *ws;

    /* The workspace which was focused when leaving the super workspace, the
     * one which was visible on the same output, or any of its workspaces. */
    TAILQ_FOREACH (ws, &(stash->nodes_head), nodes) {
        if (ws->type != CT_WORKSPACE || ws->ss_output == NULL || ws->super_workspace != num)
            continue;
        if (state->last_workspace != NULL && strcasecmp(ws->name, state->last_workspace) == 0)
            return ws;
        if (ws->ss_visible && output != NULL && strcmp(ws->ss_output, output->name) == 0)
            fallback = ws;
        else if (fallback == NULL)
            fallback = ws;
    }
    if (fallback != NULL)
        return fallback;

    /* The super workspace has no workspaces yet: create its workspace 1,
     * stashed and visible on the output of |con|. */
    ws = con_new(NULL, NULL);
    ws->type = CT_WORKSPACE;
    ws->name = sstrdup("1");
    ws->num = 1;
    ws->super_workspace = num;
    ws->ss_output = sstrdup(output != NULL ? output->name : output_primary_name(get_first_output()));
    ws->ss_visible = true;
    ws->workspace_layout = config.default_layout;
    /* Like _workspace_apply_default_orientation(), but for the output the
     * workspace will be shown on: its own output is __i3 while stashed. */
    if (config.default_orientation == NO_ORIENTATION) {
        Con *target = (output != NULL ? output : get_first_output()->con);
        ws->layout = (target->rect.height > target->rect.width) ? L_SPLITV : L_SPLITH;
    } else {
        ws->layout = (config.default_orientation == HORIZ) ? L_SPLITH : L_SPLITV;
    }
    con_attach(ws, stash, false);

    char *name;
    sasprintf(&name, "[i3 con] workspace %s", ws->name);
    x_set_name(ws, name);
    free(name);

    ipc_send_super_workspace_event("init", -1);
    return ws;
}

bool super_workspace_switch_by_name(const char *name) {
    const int num = ws_name_to_number(name);
    if (num == -1)
        return false;

    /* Remember the name for super workspaces which are not configured, so
     * that "super_workspace number 3:foo" shows up as "3:foo". */
    if (get_assignment(num) == NULL && !name_is_digits(name))
        replace_string(&(get_state(num)->name), name);

    super_workspace_switch(num);
    return true;
}

void super_workspace_back_and_forth(void) {
    if (previous_super_workspace == -1) {
        DLOG("No previous super workspace.\n");
        return;
    }
    super_workspace_switch(previous_super_workspace);
}

/* Collects the numbers of all known super workspaces (configured ones, the
 * active one and all which have workspaces), sorted ascending. */
static int collect_super_workspaces(int **nums) {
    int count = 0;
    *nums = NULL;

#define ADD_NUM(n)                                                  \
    do {                                                            \
        bool found = false;                                         \
        for (int i = 0; i < count; i++)                             \
            if ((*nums)[i] == (n))                                  \
                found = true;                                       \
        if (!found) {                                               \
            *nums = srealloc(*nums, (count + 1) * sizeof(int));     \
            (*nums)[count++] = (n);                                 \
        }                                                           \
    } while (0)

    ADD_NUM(current_super_workspace);
    struct Super_Workspace_Assignment *assignment;
    TAILQ_FOREACH (assignment, &super_workspace_assignments, super_workspace_assignments) {
        ADD_NUM(assignment->num);
    }
    Con *output;
    TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
        Con *ws;
        TAILQ_FOREACH (ws, &(output_get_content(output)->nodes_head), nodes) {
            if (ws->type == CT_WORKSPACE && (!con_is_internal(ws) || ws->ss_output != NULL))
                ADD_NUM(ws->super_workspace);
        }
    }
#undef ADD_NUM

    for (int i = 1; i < count; i++) {
        for (int j = i; j > 0 && (*nums)[j - 1] > (*nums)[j]; j--) {
            int tmp = (*nums)[j];
            (*nums)[j] = (*nums)[j - 1];
            (*nums)[j - 1] = tmp;
        }
    }
    return count;
}

static void dump_super_workspace(yajl_gen gen, int num) {
    y(map_open);

    ystr("num");
    y(integer, num);

    char *name = super_workspace_name(num);
    ystr("name");
    ystr(name);
    free(name);

    const char *user = super_workspace_user(num);
    ystr("user");
    if (user != NULL)
        ystr(user);
    else
        y(null);

    ystr("focused");
    y(bool, num == current_super_workspace);

    bool urgent = false;
    ystr("workspaces");
    y(array_open);
    Con *output;
    TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
        Con *ws;
        TAILQ_FOREACH (ws, &(output_get_content(output)->nodes_head), nodes) {
            if (ws->type != CT_WORKSPACE || ws->super_workspace != num)
                continue;
            if (con_is_internal(ws) && ws->ss_output == NULL)
                continue;
            ystr(ws->name);
            urgent |= ws->urgent;
        }
    }
    y(array_close);

    ystr("urgent");
    y(bool, urgent);

    y(map_close);
}

void dump_super_workspaces(yajl_gen gen) {
    ystr("current");
    dump_super_workspace(gen, current_super_workspace);

    ystr("notifications");
    ystr(super_workspace_notifications_all ? "all" : "current");

    ystr("super_workspaces");
    y(array_open);
    int *nums;
    const int count = collect_super_workspaces(&nums);
    for (int i = 0; i < count; i++)
        dump_super_workspace(gen, nums[i]);
    free(nums);
    y(array_close);
}

void ipc_send_super_workspace_event(const char *change, int old) {
    yajl_gen gen = ygenalloc();

    y(map_open);

    ystr("change");
    ystr(change);

    ystr("old");
    if (old == -1)
        y(null);
    else
        dump_super_workspace(gen, old);

    dump_super_workspaces(gen);

    y(map_close);

    const unsigned char *payload;
    ylength length;
    y(get_buf, &payload, &length);

    ipc_send_event("super_workspace", I3_IPC_EVENT_SUPER_WORKSPACE, (const char *)payload);

    y(free);
}
