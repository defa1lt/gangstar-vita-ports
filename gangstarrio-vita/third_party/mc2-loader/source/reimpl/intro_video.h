#ifndef MC2_INTRO_VIDEO_H
#define MC2_INTRO_VIDEO_H

/* Plays the original Android opening movie.  Every error is recoverable: a
 * missing/unsupported movie must never prevent Sandstorm 2 from starting. */
void mc2_play_intro_video(void);

/* Keeps the original Gameloft loading artwork visible while the game engine
 * performs its synchronous startup work. */
void mc2_show_loading_screen(void);

#endif
