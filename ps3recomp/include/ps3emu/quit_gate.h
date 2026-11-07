/*
 * ps3recomp - the quit gate: never end the process in the middle of a save
 *
 * A host that ends the process on its own (a window closed, a signal, the
 * in-game menu's "Quit game") must not cut a save in half. The work that
 * writes the player's data passes through the gate:
 *
 *   cellSaveData's save and load calls (libs/system/cellSaveData.c), and
 *   every guest file opened for writing, from its open to its close
 *   (runtime/ppu/ppu_fs.cpp: a checkpoint such as Resistance's quick.sav is
 *   a plain cellFs write, not a cellSaveData one).
 *
 * The quit path closes the gate first, then waits for what is inside to
 * leave. Once it is closed, a save or a write that would start parks its
 * thread for good instead -- the process is about to end, and a save that
 * never starts leaves the old one whole, where one cut short would not.
 * Entering counts first and looks at the gate second, and closing does the
 * reverse (both sequentially consistent), so a save is either seen by the
 * waiting quit or never starts.
 *
 *   ps3_quit_gate_enter / _leave   around one save, or one writable file
 *   ps3_quit_gate_close            the quit path, once: nothing new starts
 *   ps3_quit_gate_busy             saves and writable files still open
 *   ps3_quit_gate_wait(ms)         close, then wait up to ms for busy to be
 *                                  0; returns what is still busy (0 = clear)
 */
#ifndef PS3EMU_QUIT_GATE_H
#define PS3EMU_QUIT_GATE_H

#ifdef __cplusplus
extern "C" {
#endif

void ps3_quit_gate_enter(void);
void ps3_quit_gate_leave(void);
void ps3_quit_gate_close(void);
int  ps3_quit_gate_closed(void);
int  ps3_quit_gate_busy(void);
int  ps3_quit_gate_wait(int ms);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_QUIT_GATE_H */
