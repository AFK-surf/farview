# Farview Shell 3 — design rules

The shell takes its structure from Arc and its surfaces, icons and motion from the macOS Tahoe settings idiom
(grouped inset lists, colored glyph tiles, quiet captions, continuous transitions). Read this before changing any
`shell_ui` panel. `shell_frame.pion` and `shell_style.pion` are the reference implementation.

## 1. Structure

- **Frame**: the window background is one color taken from the active Space (`shell_frame_color(model)`): a light
  paper / deep graphite with ~10% of the Space accent. The docked sidebar sits straight on it with no box of its own.
- **Page**: the only solid card in the window, inset `SHELL_FRAME_GAP` (8) on every side, corner from
  `shell_canvas_radius()`, one soft shadow (`shell_card_shadow`). Split view = two cards with a gap.
- **No top toolbar while the sidebar is docked**: traffic lights, navigation and the address field live at the top of
  the sidebar. Below 720pt (or with the sidebar hidden) a 40pt toolbar row carries them.
- **Address**: shows the host only; editing opens the centered command palette.
- **On the frame** (sidebar rows, address field, favorites): translucent white washes over the tint —
  `shell_wash` (rest), `shell_wash_hover` (hover), `shell_wash_strong` + `shell_row_lift` (selected). Inks:
  `lg::surface().ink`, `shell_frame_ink_secondary`, `shell_frame_ink_tertiary`.
- **Sidebar modes** (`sidebar_modes.pion`, `library_panel.pion`): the sidebar can switch wholesale into Create a Space
  or the Library, as Arc does. The top row stays; the tabs under it crossfade with the mode's content (in over
  `MO_DIALOG_MS` with an 8pt rise, out over `MO_DIALOG_OUT_MS`; at once as built). Create a Space dims, frosts and
  blurs the page column and takes its presses (and, in the app, every browser shortcut); its theme previews on the
  frame, which eases to it and back over `MO_DIALOG_MS`. Escape (or the back arrow / Cancel) returns to the tabs; one
  owner handles Escape (the app's `dismiss_topmost_layer`, else the frame). A mode keeps the sidebar's width (the page
  never reflows) and the tab list's scroll. Below 720pt the same content fills the narrow sidebar drawer.
- **Floating panels** (drawers, popovers, menus, palettes, dialogs, sheets): one layer of `lg` glass from the `lg`
  components; inside them, content is solid (`lg::SurfaceCard`, grouped lists). Never hand-roll `Glass::liquid`.

## 2. Numbers

- Every padding, margin, gap, size and corner radius is one of the spacing scale, written as its token:
  `SP_0 SP_2 SP_4 SP_6 SP_8 SP_10 SP_12 SP_16 SP_20 SP_24 SP_32 SP_40 SP_48 SP_64 SP_80 SP_96 SP_128 SP_160`
  (sums of tokens for composite widths, e.g. `SP_160 + SP_80`). No bare literals like `7.0`, `9.0`, `14.0`, `18.0`,
  `22.0`, `26.0`, `28.0`, `30.0`, `36.0`, `44.0` in layout code.
- Package-level `pub let` constants in other files must use literals with the token in a comment
  (`pub let X : Double = 40.0 // SP_40`): package lets initialize in file order and `shell_style.pion` sorts late.
- Type sizes come from the type scale only: `TX_CAPTION 11`, `TX_FOOTNOTE 12`, `TX_BODY 13`, `TX_CALLOUT 15`,
  `TX_HEADLINE 17`, `TX_TITLE3 22`, `TX_TITLE1 28`. Weights: 400 and 500; 600 only for a panel/page title; 700 only
  for page headlines. Selection never changes weight.
- Icon glyphs: `IC_SM 12`, `IC_MD 16`, `IC_LG 20`.
- Radii: rows `SP_8`, controls `SP_8`–`SP_10`, cards `SP_12`, panels from the `lg` component; nested corners
  concentric (`lg::concentric(outer, pad)`).
- Hit targets: at least 32pt (rows 32, icon buttons 32 x 40).

## 3. Components (reuse, don't restyle per panel)

- `shell_glyph_tile(parent, icon, color, size)`: white glyph on a solid system color, the settings-icon idiom.
  Use it to give a list's categories identity (one color per kind, consistent everywhere: bookmark orange, history
  gray, search blue, tabs indigo, downloads blue, security green, privacy/permissions red, appearance purple,
  reader orange, devtools gray).
- `shell_site_tile(parent, host_by, size)`: site monogram in a stable per-host color (stands in for a favicon).
- Grouped lists (`settings_group` / `settings_row` / `settings_rule` in `shell_frame.pion` show the pattern): quiet
  caption above (`TX_FOOTNOTE`, ink_secondary, sentence case — never ALL CAPS), one solid card, rows of min 40pt with
  a 24pt glyph tile, label, trailing control (toggle, value + chevron, button), hairline rules inset to the label,
  optional footnote below. Use toggles for on/off, never "Open"/"Close" text buttons.
- `shell_frame_icon_btn`, `capsule_icon_btn`: flat icon buttons, wash on hover (`shell_fade`), 0.97 press (`shell_press`).

## 4. Motion

Tokens and helpers live in `shell_style.pion` (`MO_*`, `shell_ease_out` / `shell_ease`, `shell_spec`, `shell_go`,
`shell_fade`, `shell_reveal`, `shell_dialog_reveal`, `shell_popover_reveal`, `shell_popover_origin`, `shell_menu_node`,
`shell_press`, `shell_closes`). Use them; never hand-roll a curve or a duration.

- Judge by how often a person sees it. High-frequency acts (tab switch, Cmd+L / Cmd+T, Cmd+S, address palette, command
  bar, sidebar hover, tab close) move by opacity or a tiny scale only, at most `MO_KEY_MS` (100) when the keyboard starts
  them and 160 for the pointer, and never hold back input or content. The palette and the command bar only fade in
  and close at once. The narrow-window sidebar drawer (Cmd+S) fades in with a 12pt slide over `MO_KEY_MS` and out over
  `MO_POP_OUT_MS`; the History / Bookmarks and Spaces drawers are occasional and keep `lg`'s drawer motion.
- Occasional surfaces get standard motion: dialogs `shell_dialog_reveal` (fade + 0.98 -> 1, centered, 200 in /
  100 out); popovers, toasts and the capture sheet `shell_popover_reveal` (fade + 0.97 -> 1 from the edge facing the
  trigger or the edge they are pinned to, 140 / 100); Peek 0.97 -> 1 and its scrim over 200 with the card's opacity
  front-loaded over 100 (so the two pages are not seen through each other), transform only, never its margin or
  size; the new pane of Split View fades in over 180 (opacity only).
- One menu rule: every menu (the More menu and the bookmarks overflow through `shell_menu_node`, the page / tab /
  bookmarks-bar context menus, Back / Forward history, folder and Space popovers) comes in with
  `shell_popover_reveal` from its trigger or the pointer point, 140 in / 100 out, items readable from the first frame.
  `lg`'s menu morph (content after 170ms, done near 330ms) is not used in the shell.
- Enter and exit ease out (`shell_ease_out`); hover and color changes (washes, the Space frame tint) use
  `shell_ease`. Never start from scale 0. Exits are faster than enters.
- One hover speed: `shell_fade` washes over `MO_HOVER_MS` (120); focus washes and the selected tab over `MO_KEY_MS`.
- Press feedback on every pressable face (not list rows): `shell_press(face, pressed)` sinks it to 0.97 over 160 and
  lets it back over 100 (the release, the system's answer, is the faster one).
- Switching Space recolors only the frame tint, over `MO_KEY_MS` on `shell_ease`; the content swaps at once.
- Hover cards are instant after the first (`MO_TIP_WARM_MS`) and start at `MO_TIP_SCALE`.
- A `show(..., leave=)` only plays its exit when the close is told as a transition: wrap a person's closes from inside
  the surface with `shell_closes(v, close)`; a close the app makes itself (the toast running out) is told with
  `view::transition` too.
- Anything already true when the view is built (first mount, restored state, data arriving) appears at once (the
  `built` flag in each panel builder).
- Reduced motion is gentler, not zero: `shell_go` / `shell_fade` keep opacity and color easing (capped at
  `MO_REDUCED_MS`), scales stay 1 and slides are dropped. `lg` components still go instant under it until Invar's
  `Way::go` keeps short opacity tweens (upstream).
- Leaving releases input on its first frame.
- Evidence: `FV_SHOTS_ONLY=motion` renders `motion-<item>-<ms>.png` frames for each motion.

## 5. Slop to remove on sight

- Fake or placeholder data presented as real (metric cards, "Sync Active", lorem), self-referential marketing copy.
- ALL-CAPS section labels, `A · B · C` middle-dot metadata, counters nobody asked for ("4 tabs").
- Every row in its own bordered white box; equal-weight card grids; borders where spacing would do.
- Text buttons for state ("Open"/"Close", "On"/"Off"); duplicate controls for the same action in one view.
- Generic globe icons for every row; icons that don't say anything (decoration).
- Bare numeric literals off the scales; one-off colors (use `lg::surface()` / `shell_*` tokens).
- Hover-only controls hidden with `alpha=0` (use `show`); double separators; dangling first/last rules.
- Copy: say the object and the action, sentence case, no exclamation, no "Seamlessly"/"Effortlessly".

## 6. Evidence

- Every scene renders through `pion/cmd/shell_shots` (`FV_SHOTS_ONLY=<area>`), light and dark, 1280x800 and 480x800
  where the panel narrows. Check each image yourself against the list above before calling a change done.
- `lg::assert_apple_ui(t, label=...)` stays in every panel test.
