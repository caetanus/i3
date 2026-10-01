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
#pragma once

#include <config.h>

#include <stdbool.h>

#include <yajl/yajl_gen.h>

#include "data.h"

/** Number of the currently active super workspace (0 by default). */
extern int current_super_workspace;

/** Whether notifications of all super workspaces should be shown (the
 * default), or only the ones of the active super workspace. Only stored and
 * reported via IPC; the status bar implements it. */
extern bool super_workspace_notifications_all;

/**
 * Switches to the super workspace with the given name (only its leading number
 * is significant, e.g. "1:work"). Stashes the workspaces of the active super
 * workspace and restores the ones of the target, creating a workspace on every
 * output that ends up without one. Returns false if the name has no number.
 *
 */
bool super_workspace_switch_by_name(const char *name);

/**
 * Switches to the super workspace with the given number.
 *
 */
void super_workspace_switch(int num);

/**
 * If |con| is on a stashed workspace, switches to its super workspace.
 *
 */
void super_workspace_reveal(Con *con);

/**
 * Returns the workspace of super workspace |num| that windows moved there
 * (e.g. |con|) should go to: the one focused when it was left, the one
 * visible on the output of |con| or any of its workspaces. Creates its
 * workspace 1 if it has none.
 *
 */
Con *super_workspace_get_workspace(int num, Con *con);

/**
 * Switches to the previously active super workspace.
 *
 */
void super_workspace_back_and_forth(void);

/**
 * Returns the name of the super workspace with the given number: the name from
 * the config, the name it was last switched to with, or just the number.
 * The returned string must be freed.
 *
 */
char *super_workspace_name(int num);

/**
 * Returns the user configured for the super workspace with the given number,
 * or NULL.
 *
 */
const char *super_workspace_user(int num);

/**
 * Returns true if |con| is a window whose user is not the user of its super
 * workspace (or not the i3 user, for super workspaces without a user).
 *
 */
bool con_is_alien(Con *con);

/**
 * Dumps the super workspace state (active one, all known ones and the
 * notification scope) as the body of a JSON map.
 *
 */
void dump_super_workspaces(yajl_gen gen);

/**
 * Sends the super_workspace IPC event with the given change ("focus",
 * "notifications" or "urgent"). |old| is the previously active super
 * workspace or -1.
 *
 */
void ipc_send_super_workspace_event(const char *change, int old);
