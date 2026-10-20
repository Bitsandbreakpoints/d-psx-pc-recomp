/*
 * states.h - the list of save states, with their pictures: in the pause
 * screen (save, load) and in the launcher (load).
 */
#ifndef LAUNCHER_STATES_H
#define LAUNCHER_STATES_H

/* Reads the slots again (their headers and pictures). */
void states_refresh(void);
void states_free(void);

/* TRUE if some slot has a state this program can load. */
int states_any_loadable(void);

/* TRUE if some slot has a state at all (perhaps one that cannot be loaded:
 * the list says why). */
int states_any(void);

/* states_list() modes */
enum StatesMode {
    STATES_SAVING,                      /* every slot (an empty one, or one to overwrite) */
    STATES_LOADING,                     /* those that can be loaded */
    STATES_BROWSING                     /* those with a state, which can also be deleted */
};

/*
 * The slots as rows (picture, number, disc, time played, date) in the
 * current Dear ImGui window, `width` wide, in an enum StatesMode.
 * `focus`: the keyboard / controller selection goes to the first row that
 * can be chosen.  Returns the slot chosen, else -1.  Browsing, each state
 * has a Delete button, and Delete or X (Square) on its row asks for it
 * too: `*remove` is then that slot (else left as it is).
 */
int states_list(int mode, float width, float row_height, int focus, int *remove);

/* "Disc 2, 1:02:15 played" for slot `slot` (empty if there is none). */
void states_describe(int slot, char *out, int size);

#endif /* LAUNCHER_STATES_H */
