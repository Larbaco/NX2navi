// The look of the game's own menus (ui/menu_skin.cpp): the save menu's brown button with its inset line and corner
// ornaments, the options menu's white selection cursor and its filigree divider, and a dark panel framed like the
// buttons. Each is drawn from the game's textures when they are there (ui/menu_art.cpp), else from plain shapes in the
// same colours. Sizes are in pixels at 1080 lines; s scales them (menu.cpp menu_scale).
#pragma once
#include <stdint.h>

#include "imgui.h"

// What the art needs after it is decoded: the cursor's frame is cut out of its texture here (art: a MenuArt).
void skin_art_prepare(int art, uint8_t *rgba, uint32_t w, uint32_t h);
// One level of an art at half the size of the last: dst is dw x dh (each half of the source's, at least 1).
void skin_art_halve(const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t dw, uint32_t dh);

// The settings menu's Dear ImGui style at 1080 lines: the colours of the game's menus (a dark umber panel, warm browns
// for frames and what is selected, gold ornaments and accents, parchment text) and the sizes of its frames.
void skin_style(ImGuiStyle &s);

// The boxes' mottled grain lives in the font atlas: skin_atlas_add before the atlas is built (it reserves the tile),
// skin_atlas_fill after it is built and before it is uploaded (it draws the tile). Without them the boxes are plain.
void skin_atlas_add(ImFontAtlas *fa);
void skin_atlas_fill(ImFontAtlas *fa);

// The brown button over [a, b]: a dark rim, a light edge, the fill, and an inset line that stops short of a diamond at
// the top-left and bottom-right corners and of a curl at the other two. lift 0..1 brightens the fill (hovered or
// selected); dim mutes it (a row the environment fixes).
void skin_button(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float lift, bool dim);
// How far skin_cursor reaches outside the box it surrounds: a box needs that much room on every side.
float skin_cursor_reach(float s);
// The selection cursor around the box [a, b], in two parts: a soft white-blue glow, drawn before the box (the box hides
// its middle, so it shows only around it), and a white frame a little outside the box, drawn after it. Both breathe
// slowly; alpha 0..1 (a hovered row's cursor is fainter than the selected one's).
void skin_cursor_glow(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float alpha);
void skin_cursor_frame(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float alpha);
// The divider: a thin line with a dot at each end and the ornament in its middle, centred on y between x0 and x1.
void skin_filigree(ImDrawList *dl, float x0, float x1, float y, float s, float alpha = 1);
float skin_filigree_height(float s);
// A dark panel over [a, b] with the button's edge, inset line and corner ornaments (the description pane, the
// performance overlay). alpha scales its fill.
void skin_panel(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float alpha = 1);

// The boot screen's box (menu.cpp menu_boot_frame) centred in a w x h frame: the panel, the title in gold over the
// divider, the step's line, a bar filled to frac (0..1; a negative frac: a short segment sweeping at position sweep
// 0..1, no count yet).
void skin_boot_panel(ImDrawList *dl, ImFont *head, float head_px, ImFont *body, float body_px, const char *title,
                     const char *step, float frac, float sweep, float w, float h, float s);

// A button widget drawn as skin_button with its label centred and skin_cursor round it while selected or hovered (the
// save menu's Load and Cancel). True when pressed (click, A, Enter). size: as for ImGui::Button.
bool skin_widget_button(const char *label, ImVec2 size, float s);

// One tab of the bar: ImGui::BeginTabItem drawn as the rows' button, with the game's plate behind the selected tab's
// lines and its text in gold. id: BeginTabItem's label (it sizes the tab); text: what is shown. font: the label's font,
// NULL for the current one. True when selected: call ImGui::EndTabItem then.
bool skin_tab_item(const char *id, const char *text, int flags, float s, ImFont *font);

// The colours the boxes are drawn in, for text on them.
#define SKIN_TEXT IM_COL32(244, 236, 214, 255)       // labels on a button
#define SKIN_TEXT_DIM IM_COL32(206, 188, 150, 255)   // notes on a button
#define SKIN_GOLD IM_COL32(236, 196, 104, 255)       // headings and accents
