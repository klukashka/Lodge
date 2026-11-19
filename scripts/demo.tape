# Rebuilds demo.gif. Run from the repository root after `make`: vhs scripts/demo.tape
# The window is 90x44 so the sheep and the sun are big. EVENT_SHIFT puts the sheep on a bare patch of the bank, grazing
# for the whole clip; the sky and the events run at the normal speed (the sun stands at about 17:00, clear of the mountains).
Output demo.gif
Set Shell "bash"
Set FontSize 17
Set Width 950
Set Height 1000
Set Padding 0
Set Framerate 12
Set Theme "Builtin Dark"
Hide
Type "clear" Enter
Type "LODGE_SEASON=summer LODGE_AT='2026-07-01 16:59' LODGE_EVENT_SHIFT=41274725 LODGE_WIND_DIR=right LODGE_BIN=./lodge LODGE_TEXTS_DIR=./texts bash scripts/lodge" Enter
Sleep 19s
Show
Sleep 16s
