# Fonts of the LVGL interface

`tools/mkfonts.sh` reduces them to the characters of the texts and writes them as TTF data into
`src/fonts/orm_font.c`, which is committed, so a build needs no fontTools. LVGL's Tiny TTF renders them in
the size of the screen.

- `DejaVuSans.ttf`: DejaVu Sans 2.37, the font of ORM, license in `DejaVuSans-LICENSE.txt`. Kept
  are Latin-1 and Latin Extended-A, modern Greek and Cyrillic, Arabic and Persian with the forms
  LVGL joins, see `tools/mkfonts.sh`.
- `fontawesome/fa-solid-900.ttf`: Font Awesome 7.3.1 Free Solid, converted unchanged from
  `webfonts/fa-solid-900.woff2` of https://github.com/FortAwesome/Font-Awesome (tag 7.3.1). License in
  `fontawesome/LICENSE.txt` (fonts: SIL OFL 1.1). The reduced font holds only a few of its icons and has
  no names, so it is not named Font Awesome, its Reserved Font Name.
