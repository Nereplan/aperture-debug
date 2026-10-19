# Aperture hit registration and keyboard audit — interrupted snapshot

**Status: incomplete; Computer Use stopped at the user's request.** This report records the checks completed on 11–12 October 2026. It is not a claim that every button, preset, or keyboard path has been tested. No application code was changed, built, or patched. The testing goal was paused on 12 October so a new run can start with fresh context.

## Build, workload, and evidence

| Item | Evaluated state |
| --- | --- |
| Executable | `release/Aperture.exe`, x64 Release, version **2.0.0.0** |
| Executable size / filesystem modification time | 3,265,536 bytes / 2026-10-10 03:23:20 |
| SHA-256, read from the executable and matching its manifest | `e09bcd44fdd7f69f068e62b36a71b572645e2e02930a9a5b11b4de1328d0da25` |
| Source checkout inspected | `cc006c5526eb98d588f86b73e92d2445ae3d1d7a` |
| Source provenance limitation | The release manifest does not identify its source commit. The checkout revision is a reference for code reading, not proof that the binary was compiled from that exact revision. |
| Primary media | `D:\Movies\DanMachi\[Kayoanime] DanMachi - S05E01.mkv`; 231.1 MB; 23:41; HEVC video; two audio tracks and two English subtitle tracks; no chapters exposed |
| Additional fixture | Episodes S05E02 and S05E03, added through the native picker to a temporary three-item playlist |
| Display configurations exercised | Normal window 1455 × 1025; fullscreen 1920 × 1080; PiP 640 × 360 |
| Automation method | Windows Computer Use skill, `@oai/sky`; actual cursor clicks, drags, wheel input, and keyboard input; immediate screenshot/accessibility observation after each action |
| Persisted evidence | **427 logged steps**, including navigation and two explicitly logged observations; the complete compact step list is appended below |
| Raw observation log | `C:\Users\Alperen\AppData\Local\Temp\aperture-ui-audit-20261011.json` — action parameters, timestamps, accessibility snapshots, and coverage notes |

The first saved log ends its individually recorded 11 October actions at Contrast Back. Additional picture, subtitle, safe-area, and profile checks were observed afterward; their results survive in the continuation coverage notes, but their individual action records were lost when the session went cold. Those checks are marked **session note** below.

Screenshots were inspected live, including settled observations when animation or loading made the first capture ambiguous. Audit screenshots were not saved as image artifacts. The user's supplied speed-menu reference is at `C:\Users\Alperen\.codex\attachments\80feacdf-ca56-4a8b-b8ec-9931d14d6afc\image-1.png`.

**Meaning of Pass:** the exercised input registered on the intended control and produced the observed UI/state response. This does not certify every semantic consequence, audio signal transformation, exact decoded frame, timer expiration, or external integration.

## Findings

| ID | Priority / evidence strength | Finding |
| --- | --- | --- |
| F01 | P1 — reproduced | Space reopens player Settings after Settings has been opened once, including after clicking the video to refocus and after clicking Play. |
| F02 | P2 — reproduced | The player speed slider accepts clicks but does not respond to two tested drags. |
| F03 | P2 — reproduced; source corroboration | The configured `[` / `]` speed shortcuts execute A–B repeat commands instead. |
| F04 | P2 — session note; visual and accessibility observation | Save Subtitle Style Profile opens an invisible modal over the video; keyboard input and Enter still save a profile. |
| F05 | P2 — reproduced; source corroboration | Mark as watched changes the item to Done, but the single-item menu still offers Mark as watched rather than Mark as unwatched. |
| F06 | P2 — observed; further verification advisable | F5, advertised on Refresh, produces no observed refresh response; clicking Refresh produces Library Refreshed. |
| F07 | P2 — observed with Computer Use drags | Dragging the PiP video surface toggles play/pause instead of moving the window. |
| F08 | P2 — observed; keyboard retest needed | Player flyout Tab/Down/Up navigation did not move the first-row highlight; Enter after Down opened Subtitles instead of Audio. |
| F09 | P3 — reproduced visual state | Back stays blue with the pointer below the speed presets. Clicking the blank area does not activate Back or 1×. |
| F10 | P3 — reproduced | Diagnostics reports AudioOnly/NoVideo while a video picture is visibly rendering. |
| F11 | P3 — observed | Custom seek/frame input dialogs have a blank explanatory area despite an accessibility prompt; input and confirmation work. |
| F12 | P2 — conditional failure | Enabling custom MPV configuration from Hub before media initialization fails with mpv player is not initialized and rolls the switch back off. |
| F13 | Review — observed behavior | Folder removal and bulk playlist-item removal occur immediately and are not undone by Ctrl+Z. Single-media removal has a confirmation. |
| F14 | P3 — accessibility defects | Modal descriptions retain text from a previous unrelated confirmation; many visible shortcut capture buttons are absent from the accessibility tree. |

### F01 — Space retains the Settings action

1. Open a video and press Space before opening player Settings: playback pauses correctly.
2. Click the Settings gear, then close its flyout with Escape.
3. Press Space: Settings reopens; playback remains paused at 05:33.
4. Close Settings, click the video picture, and press Space: Settings still reopens and the time remains unchanged.
5. Start playback with the Play button and press Space: Settings opens while playback continues.

Expected: Space resumes its normal play/pause behavior after the flyout closes and after a video refocus click. Evidence: steps **120–125, 198–201**. PiP Space paused correctly in the same run (**232**), so the observed problem is not universal to all player modes. A fresh player session on 12 October also accepted Space before Settings (**377**).

### F02 — speed slider drag fails while clicks work

In the normal 1455 × 1025 window, open Settings → Speed:

- At 3.00×, drag `(1410, 693)` to `(1178, 693)`: speed stays 3.00× after a settled observation.
- Click `(1255, 693)`: speed becomes 1.15×.
- Drag the interior thumb from `(1254, 693)` to `(1360, 693)`: speed stays 1.15× after settling.

Expected: dragging changes speed continuously toward the destination. Evidence: **146–149**. Timeline, normal volume, Hub subtitle-size, PiP volume, and playlist-reordering drags worked, providing positive controls for the input method.

### F03 — speed shortcut collision with A–B repeat

The shortcut page displays `[` for speed down and `]` for speed up. In the player, `[` sets repeat point A. `]` reports Set A before B or sets point B and enables repeat, depending on the current position. Speed remains 1.00×. Seeking forward before `]` produces an active A–B repeat. Backslash clears repeat successfully.

Evidence: **207–212**. Read-only inspection of `src/AppKeyboard.cpp` found hard-coded bracket repeat handling before configured command dispatch. The six delay shortcuts and Backspace speed reset did respond to their assigned commands.

### F04 — profile modal is invisible over video

From the player context menu, open Subtitles & Dual Subtitles → Style Profiles → Save Current Style as Profile. After another settled capture, the video remains visible with no profile dialog drawn. The accessibility tree contains Save Subtitle Style Profile, a name input, OK, and Cancel. No secondary app window exists in the window inventory.

Ctrl+A, typing `UI Audit 2026-10-11`, and Enter update the input and produce Profile Saved. The profile appears on reopening Style Profiles. Restore Default Style and applying the saved profile respond correctly. Thus the modal is operational through the keyboard while its visual surface is missing. **Session-note evidence; individual post-347 actions were not persisted.** Rename and delete were not completed.

### F05 — watched toggle cannot switch back through its single-item row

Right-click S05E01 and choose Mark as watched. The item displays Done and its resume value becomes 23:41. Reopen the menu: its row still reads Mark as watched. It remained that way after the tested F5 input. The distinct background bulk Mark Selected as Unwatched command works and resets progress to 00:00.

Evidence: **382–389**. Code reading corroborates a mismatch: the single-item context menu checks `item.status == "watched"` in `src/AppMenus.cpp`, while `SetWatchedCore` stores `"done"` in `src/MediaLibrary.cpp`. No fix was made. A mouse refresh was subsequently exercised after the bulk unwatched action; the single-item watched menu was not retested after that mouse refresh.

### F06–F08 — observations requiring a focused follow-up

- **F5:** keyboard F5 produced no toast or visible refresh change (**386**); the background Refresh row, labeled F5, produced Library Refreshed when clicked (**390–391**). The source keyboard handler inspected did not expose a `VK_F5` case. Retest with a change that makes refresh observable.
- **PiP dragging:** two surface drags, including an up-left drag, left the window origin at `(1280, 672)` and changed play/pause (**235–238**). This is a failure with the documented Computer Use drag method; confirm with a fresh PiP window and a visible window-move target.
- **Flyout keyboard:** Tab, Down, Shift+Tab, and Up left the first option highlighted; Enter after Down opened Subtitles, and Left returned to the main flyout (**180–186**). Retest with the cursor parked outside the flyout so mouse hover cannot interfere with keyboard highlight. Do not treat this as a fully isolated keyboard defect yet.

### F09 — reference-like highlight, without proven shifted click regions

In the speed submenu, clicking blank space at `(1275, 873)` leaves the submenu open and speed at 1.00×. Back remains blue while the pointer is below the presets; 1× is also blue as the selected preset (**127**). This resembles the supplied reference image, but the blank click does **not** activate either control. All nine speed preset clicks select their intended values.

The evidence supports stale hover/focus presentation. It does not prove that both controls have displaced active click regions or that the whole window has a coordinate offset. Cursor interpolation visible in immediate captures was not counted as an app hit-region defect.

### F10–F14 — other concrete observations

- **Diagnostics:** I opens/closes the overlay, but Resolution reads AudioOnly/NoVideo while HEVC video is rendering; the overlay also reports a video decoder and 23.976 fps (**247, 249**).
- **Input modals:** custom seek and frame values can be selected, entered, and confirmed, but explanatory text is not drawn as expected (**28–31, 52–55**).
- **MPV configuration:** the Hub enable attempt before opening media produces Custom MPV Config Failed: mpv player is not initialized, leaving it disabled (**58**). Enabling after initialization remains untested. Edit File produces an opening notification but no editor was observed; file-association success is unverified (**63**).
- **Removal:** Remove Folder from Library… immediately removed the one-item library; immediate Ctrl+Z reported Nothing to undo (**371–372**). The same original file was restored through the picker, and 05:05 was restored through the timeline (**373–379**). Later watched/unwatched tests changed that position again. Removing three entries from the temporary playlist also happened immediately, and Ctrl+Z reported Nothing to undo (**421–422**); the three entries were re-added (**423–426**). This is an observed consistency/undo limitation, not a displaced-pointer finding. Source media files were not deleted.
- **Accessibility:** Save Subtitle Style Profile inherited the previous Reset Preferences description. Create New Playlist inherited Remove from Library's description (**397–400**). On the shortcut page, the lower left rows and the entire right column were visible but missing from the inspected accessibility tree; physical pointer targeting remained available.

## Completed coverage

### Hub settings

| Area | Controls / presets exercised | Observed result and restoration |
| --- | --- | --- |
| Appearance | Dark, Black/OLED, Slate; English (US) language button | Themes pass; Dark restored. English is the only exposed language choice; button responds. |
| Playback toggles | Autoplay, remember frame/position, hardware decoding, loop; both states | Pass; original On/On/On/Off states restored. |
| Seek step | 1, 2, 5, 10, 15, 30, 60 seconds; custom 7.5; Ctrl+A; Enter | Values register; 5 seconds restored. F11 applies to custom dialog text. |
| Default speed | 0.5, 0.75, 1, 1.25, 1.5, 2× | Pass; 1× restored. |
| Frame step | 1, 2, 5, 10, 24, 30, 60 frames; custom 3; mouse OK | Values register; 1 restored. F11 applies to custom dialog text. |
| MPV actions | Enable; Edit File; Reset; Revert | Enable fails before media initialization. Reset/Revert dialogs and cancellation respond; destructive confirmations were not executed. Editor launch unverified. |
| Preferred audio | English, Japanese, French, German, Spanish, Italian, Default | All buttons respond; Japanese restored. |
| Preferred subtitles | English, Japanese, French, German, Spanish, Italian, Off | All buttons respond; English restored. |
| Subtitle size | Clicks/drags at 14, 20, 26, 32 px | Pass; 20 px restored. |
| Subtitle color | Lavender `#cad3f5`, yellow, green, cyan, orange, pink | All six respond in preview; lavender restored. |
| Subtitle background | Outline, translucent box, solid box, clean text | All four respond; Outline restored. |
| Subtitle position | Top, elevated, bottom; scrolling to controls | All three respond; Bottom restored. Wheel scrolling works. |
| Shortcuts | Play/Pause capture; M conflict with Mute; conflict Cancel; Reset dialog + Escape | These paths pass; no binding changed. The other 32 capture buttons remain untested. |
| System toggles | Hover previews, reduced motion, smart series detection; both states | Pass; original On/Off/Off states restored. |
| Storage / reset | Open Data, Clear Cache, Clear History, Reset Preferences | Open Data reveals `release/data`; test Explorer closed. Cache reports 0.0 MB freed. History/Preferences confirmation opening and cancellation pass; full resets not executed. |
| Update | Update Aperture clicked once | Invocation only. No restart or completed update observed; end-to-end result unverified. Code inspection indicates an external updater process. |

### Player controls and shortcuts

| Area | Exercised behavior | Result / boundary |
| --- | --- | --- |
| Transport | Play/Pause mouse buttons; Space; Back button | Mouse transport and Back pass. Space fails after Settings, F01. |
| Timeline | Midpoint click; drag to roughly 75%; left/right endpoints | Seek registers correctly. Right-edge test was about one pixel short of the edge and displayed 23:40/23:41. Clicking the timestamp label did nothing; no editable timestamp path was established. |
| Seeking | Left/Right ±5 seconds; J/L ±10 seconds | Corresponding time changes observed. Other configured seek-step values were selected in Hub, not each replayed as a seek. |
| Volume | Up/Down, M, mouse mute/unmute, slider drag to 0, click 50, restore 100 | Pass. |
| Track cycling | S and CC; A; flyout primary Off/two English tracks and two audio tracks | All exposed track choices/cycles respond. Japanese audio and second English subtitle restored during the first session. |
| Speed | All nine flyout presets: 0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 3×; slider click/drag | Presets and click pass; 1× restored. Drag fails, F02. |
| Speed shortcuts / repeat | `[`, `]`, Backspace; set A, seek, set B, clear with backslash | Brackets collide with repeat, F03. Backspace and repeat clear respond. |
| Frame commands | Period and comma while paused | Command response/toast observed. Exact one-frame image advancement was not quantified. |
| Delay shortcuts | Ctrl+Right, Ctrl+Left, Ctrl+Backspace; Ctrl+Shift+Right, Ctrl+Shift+Left, Ctrl+Shift+Backspace | Audio ±50 ms and subtitle ±100 ms/reset responses pass; zero restored. |
| Aspect | Auto, 16:9, 4:3, 21:9, Stretch to Fill, Zoom/Crop; X | All six choices and X respond. Auto restored after preset cycle; a later X test left 16:9 before the cold-session interruption. Relaunch state not explicitly rechecked. |
| Flyout switches | Loop, Autoplay, Private Session, clicked via switch and row | Respond and restore Off/On/Off. |
| Empty queue flyout | Open and Back | Pass; populated queue selection not yet tested. |
| Fullscreen | F enters; fullscreen toolbar button exits | Pass. Other window sizes/DPI and window-chrome controls not tested. |
| PiP | P enters; restore button; pin/unpin; volume open/close; volume drag/click; Play; Space | These respond. Disabled Previous/Next stay inactive without a queue. Surface dragging fails as observed in F07. Populated queue navigation, close, and resizing not completed. |
| Next / previous | N and Page Up in the one-item library | Reload/wrap behavior observed on the same item; distinct-target navigation unverified. |
| Chapters | Page Down and Shift+P | No Chapters response with the primary fixture. Actual chapter jumps need chapter-bearing media. |
| Other shortcuts | R restart; I diagnostics; Ctrl+S screenshot; user's custom 0 Private Session binding | Commands respond. Screenshot success reported by the app after a delayed observation; file not independently inspected. Diagnostics has F10. Private Session restored Off. |
| Flyout keyboard | Tab, Shift+Tab, Up, Down, Left, Enter, Escape | Left/Enter/Escape respond; selection movement needs retest, F08. |

### Advanced player menus

| Area | Every preset / action tried within this area | Result |
| --- | --- | --- |
| Content speed | Cinema/TV 1.00×; Audiobook 1.20×; Lecture 1.25×; Podcast 1.50×; Review/Skim 2.00× | All five report matching speeds; 1.00× restored. |
| Sleep | 15, 30, 60, 90 minutes; Stop After Current; Cancel Timer | Assignments and cancellation respond. Cancel clears timer/stop-after-current. Actual timer expiration was not awaited. |
| Audio delay menu | Increase +50 ms; Decrease −50 ms, including negative; Reset; Back | Pass; 0 restored. Back returns the root player menu, not More Options. Step 308's original label is corrected by its saved result. |
| Audio balance | Shift left/right 10%; full left; center; full right; Back | All five settings and Back respond; center restored. Audio channel output not measured. |
| Channel mapping | Auto, Stereo Direct, Mono Left, Mono Right, Swap Left/Right; Reset to Auto; Back | All five presets/reset/Back respond; Auto restored. Audio signal routing not measured. |
| Subtitle delay menu | Increase +100 ms; Decrease −100 ms; Reset; Back | Pass; 0 restored. |
| Rotation | 90°, 180°, 270°, 0° through Video Adjustments | Pass; visible video rotation and values observed. The separate More Options Rotation row was not clicked. |
| Brightness / contrast | Each: +5, −5, Reset 0, Back | Pass; zero restored. |
| Saturation / gamma / hue | Each: +5, −5, Reset 0, Back; Reset All Adjustments | Pass; zero restored. **Session note.** |
| Context primary / secondary subtitles | Each: Off and both exposed English tracks | All six choices respond; primary second English and secondary Off restored. **Session note.** |
| Named subtitle safe-area presets | Off 0%; Action Safe 5%; Title Safe 10%; Wide/Large 15%; Cinema 20% | All five respond. **Session note.** |
| Independent safe-area margins | Horizontal and vertical, each 0, 5, 10, 15, 20, 25%; Reset; Back | All twelve values/reset/Back respond; Off restored. **Session note.** |
| Style profiles | Save temporary profile via Ctrl+A/type/Enter; Restore Default; apply saved profile | Commands respond, but Save modal is invisible, F04. **Session note.** Rename/delete pending. |

### Hub Media and temporary playlist

| Area | Completed checks | Result / boundary |
| --- | --- | --- |
| Media filter | Ctrl+F; no-match text; Clear Search button | Correct empty result and restored full list. |
| Media sort | Title, Date Added, Last Played, Duration; Ascending, Descending | All choices update labels; Title ascending restored. Tested when only one item existed, so actual multi-item ordering is not verified. |
| Media grouping | Folders, chevron collapse/expand, Standalone | Pass; Standalone restored. |
| Folder menu | Rediscover/Reconnect; Open Folder in Explorer; Remove Folder | Rediscovery responds without adding new entries. Explorer reveals parent `D:\Movies` with DanMachi selected rather than entering DanMachi. Removal/undo behavior is F13. Folder Play All not completed. |
| Media context/background | Mark watched; bulk mark unwatched; Refresh; single Remove confirmation + Cancel; Escape dismissal | Intended rows register. F05/F06/F13 apply. Remaining menu actions are listed below. |
| Native media picker | Empty-library Open File; filename entry; Open mouse confirmation | Restores the original file. The earlier double-click Media opening also passes. |
| Playlist creation | Plus button; Ctrl+A/name input; mouse OK | Creates `UI Audit 2026-10-12`. |
| Playlist file import | Add Files; three quoted filenames; Enter confirmation | Three entries appear; Import complete — 3 imported. |
| Playlist layouts / filter | Grid, List; Ctrl+F; filter S05E02; Escape clear | Pass; List restored. |
| Playlist reordering | Drag first after third, then back to first | Order changes 1/2/3 → 2/3/1 → 1/2/3. Pass. |
| Playlist keyboard selection | Down, Shift+Down, Home, Ctrl+A | Down and Shift+Down move a single selection; Ctrl+A selects all three, confirmed by context count. Home did nothing. Source playlist-specific navigation does not implement Home or Shift range expansion, so these are recorded behavior, not proven regressions. |
| Playlist bulk commands | Mark all three watched; mark all three unwatched; remove all three; Ctrl+Z; re-add | Watched counts/state update correctly. Removal works; undo does not, F13. Fixture restored. |
| Playlist Play All | Click Play All with three entries | Opens S05E01 with queue controls; Previous disabled, Next enabled. The subsequent pause attempt was interrupted by the user's physical Escape. No further queue execution was tested. |

## State left for the next run

This is the **last observed state**, not a new live inspection. No Computer Use was performed after the stop request.

- The temporary playlist **UI Audit 2026-10-12**, ID `pl_1791797586_1`, contains S05E01/S05E02/S05E03 in that order. Favorites remained empty and was not edited.
- The library has imported fixture entries for S05E02 and S05E03 in addition to the original media path. No source media file was changed or deleted.
- The temporary subtitle style profile **UI Audit 2026-10-11** was saved in the earlier session and had not been renamed/deleted when that session ended. Its persistence after relaunch was not rechecked.
- S05E01's initial resume was approximately **05:05**. That position was restored once after the folder-removal test, but later bulk watched/unwatched and playlist import/playback tests changed progress. **Do not assume final resume/history state is restored.** Fixture episode progress/history may also need cleanup.
- Last completed action: playlist Play All opened S05E01 and reported Playing. The attempted Space pause returned “Computer Use was stopped by the user with the physical Escape key.” Final playback/window state after that physical action is unknown.
- The app reported a screenshot under `./screenshots/` with the S05E01 name and timestamp `20261011_130930`. It was not deleted or independently inspected.
- All deliberate Hub preference toggles/presets were restored during their tests. Player speed, balance, channel mapping, delays, picture adjustments, safe area, loop/autoplay/private state were restored during the first session. Exact current player state after relaunch/stop was not audited.
- Clear Cache was invoked and reported 0.0 MB freed. Update was invoked once; completion was not established. These actions should not be silently repeated during handoff.

## Remaining coverage — do not treat this snapshot as completion

1. **Hub History:** inspect its visible controls and every available context action, keyboard navigation, selection, removal, and undo behavior using fixture records. The History tab's full feature set was not exercised.
2. **Hub Media:** populated sort ordering; matching filter/text-edit keyboard paths; remaining single/multi/background commands; folder Play All; toolbar/global file and folder shortcuts; native folder picker; watched-folder add/rescan/stop; missing-file and smart-series paths where available.
3. **Playlists:** remaining rail/context controls; open/play/resume variants; F2 and rename dialog/inline editing; rail dragging; individual remove/trash and advertised undo; individual watched/unwatched commands; import/export; duplicate add; smart-playlist rule controls where reachable; delete the audit playlist through the UI when finished.
4. **Player root/More menus:** physically click the remaining root actions even where keyboard counterparts passed; Restart, bookmark creation/navigation/deletion, context A/B/Clear, separate Rotation row, Show Last Screenshot, and remaining Back/Exit paths.
5. **Subtitle tools:** Open Subtitle File and its picker, online subtitle/provider controls, subtitle-delay entry from its alternate menu, profile rename/delete, and any conditional options exposed by external subtitles.
6. **Populated queues:** Next/Previous by mouse and assigned shortcuts; queue flyout item selection; queue boundaries/loop behavior; autoplay end transition; PiP Previous/Next with a queue. Play All was opened, but these tests were not reached.
7. **Keyboard completeness:** the other 32 shortcut capture buttons and cancel/conflict handling; player Escape exit; global Ctrl+O/Ctrl+Shift+O/Ctrl+L/import/export/help paths; player Tab focus order and focused-control Enter/Space; all supported Hub navigation zones. Retest F01/F08 with the cursor clearly outside transient controls.
8. **Conditional fixtures:** find chapter-bearing media and verify chapter jumps; exercise a live/network source if appropriate; validate MPV enable after initialization. The primary fixture cannot prove these paths.
9. **Geometry and edge hits:** normal/maximized/restored/minimized windows, window chrome and resizing; PiP close/resize/move; boundary clicks around Back, Speed presets, gear, and sliders at additional window sizes. No DPI-change test was performed.
10. **Restoration and finish:** remove only audit-created playlist/profile/fixture history/library entries as appropriate; restore the original media's 05:05 position and preferences through UI; verify the final app state; complete the coverage inventory before claiming every expected preset/button/keyboard path is tested.

## Evidence limitations

- The individually persisted log covers 427 steps, not 427 distinct controls. Reopening menus, restoring values, and navigation count as steps.
- The post-347 11 October checks have session-note evidence rather than individual raw snapshots. A new run should recheck high-impact findings that rely on those notes.
- No wall-clock timer expiry, exact frame-count measurement, audio-routing measurement, update completion, external editor launch success, or screenshot file content was established.
- Default delay/seek commands were exercised; changing every Hub seek/frame preset did not include replaying a command under every value.
- Human Ctrl/Shift+mouse-click combinations were not exercised by a modifier-held click API. Ctrl+A and keyboard navigation provided the tested multi-selection paths.
- Source code supplied expected behavior and corroboration only. Binary/source mismatch is possible; no source-level test was substituted for a physical UI test.

## Persisted step log

The table below is generated from the saved action log. Times use **Europe/Istanbul (UTC+03:00)**. Coordinates are relative to the screenshot used for that action; native picker actions using `screenshot-1` refer to that picker image, not the main app image. Labels describe intended tests, not automatic pass/fail judgments. The findings and coverage tables above record assessed outcomes.

<!-- BEGIN PERSISTED STEP LOG -->
| Step | Local date/time | Test / action | Input |
| --- | --- | --- | --- |
| 001 | 10-11 12:35:17 | Hub Playlists physical coordinate click | Selected Playlists from Media |
| 002 | 10-11 12:35:17 | Hub Settings coordinate click | Click (1269, 64) |
| 003 | 10-11 12:35:44 | Appearance preset Black (OLED) | Click (1100, 260) |
| 004 | 10-11 12:35:54 | Appearance preset Slate | Click (530, 315) |
| 005 | 10-11 12:36:06 | Appearance preset Dark restore | Click (545, 260) |
| 006 | 10-11 12:36:32 | Display language English (US) button | Click (462, 405) |
| 007 | 10-11 12:36:40 | Hub Playback settings category | Click (117, 198) |
| 008 | 10-11 12:37:04 | Autoplay toggle off | Click (1380, 267) |
| 009 | 10-11 12:37:11 | Autoplay toggle on restore | Click (1380, 267) |
| 010 | 10-11 12:37:15 | Remember position toggle off | Click (1380, 314) |
| 011 | 10-11 12:37:20 | Remember position toggle on restore | Click (1380, 314) |
| 012 | 10-11 12:37:26 | Hardware decoding toggle off | Click (1380, 359) |
| 013 | 10-11 12:37:30 | Hardware decoding toggle on restore | Click (1380, 359) |
| 014 | 10-11 12:37:34 | Loop toggle on | Click (1380, 406) |
| 015 | 10-11 12:37:38 | Loop toggle off restore | Click (1380, 406) |
| 016 | 10-11 12:37:49 | Seek step preset dropdown | Click (1255, 486) |
| 017 | 10-11 12:38:02 | Seek preset 1 second | Click (1315, 506) |
| 018 | 10-11 12:38:07 | Reopen seek preset menu for 2 seconds | Click (1250, 486) |
| 019 | 10-11 12:38:11 | Seek preset 2 seconds | Click (1315, 538) |
| 020 | 10-11 12:38:17 | Reopen seek preset menu for 10 seconds | Click (1250, 486) |
| 021 | 10-11 12:38:21 | Seek preset 10 seconds | Click (1315, 601) |
| 022 | 10-11 12:38:28 | Reopen seek preset menu for 15 seconds | Click (1250, 486) |
| 023 | 10-11 12:38:32 | Seek preset 15 seconds | Click (1315, 633) |
| 024 | 10-11 12:38:43 | Reopen seek preset menu for 30 seconds | Click (1250, 486) |
| 025 | 10-11 12:38:48 | Seek preset 30 seconds | Click (1315, 666) |
| 026 | 10-11 12:38:53 | Reopen seek preset menu for 60 seconds | Click (1250, 486) |
| 027 | 10-11 12:38:57 | Seek preset 60 seconds | Click (1315, 698) |
| 028 | 10-11 12:39:04 | Seek Custom button | Click (1358, 486) |
| 029 | 10-11 12:39:11 | Custom seek select-all | Ctrl+a |
| 030 | 10-11 12:39:17 | Custom seek type 7.5 seconds | Type: 7.5 |
| 031 | 10-11 12:39:22 | Custom seek confirm Enter | Return |
| 032 | 10-11 12:39:28 | Reopen seek menu for default restore | Click (1250, 486) |
| 033 | 10-11 12:39:32 | Seek preset 5 seconds restore | Click (1315, 570) |
| 034 | 10-11 12:39:39 | Default playback speed preset 0.5x | Click (364, 680) |
| 035 | 10-11 12:39:43 | Default playback speed preset 0.75x | Click (556, 680) |
| 036 | 10-11 12:39:48 | Default playback speed preset 1.25x | Click (935, 680) |
| 037 | 10-11 12:39:52 | Default playback speed preset 1.5x | Click (1127, 680) |
| 038 | 10-11 12:39:58 | Default playback speed preset 2x | Click (1318, 680) |
| 039 | 10-11 12:40:03 | Default playback speed preset 1x restore | Click (745, 680) |
| 040 | 10-11 12:40:11 | Frame step preset dropdown | Click (1250, 572) |
| 041 | 10-11 12:40:17 | Frame preset 2 | Click (1310, 623) |
| 042 | 10-11 12:40:21 | Open frame menu for 5 | Click (1250, 572) |
| 043 | 10-11 12:40:26 | Frame preset 5 | Click (1310, 655) |
| 044 | 10-11 12:40:31 | Open frame menu for 10 | Click (1250, 572) |
| 045 | 10-11 12:40:35 | Frame preset 10 | Click (1310, 687) |
| 046 | 10-11 12:40:41 | Open frame menu for 24 | Click (1250, 572) |
| 047 | 10-11 12:40:46 | Frame preset 24 | Click (1310, 719) |
| 048 | 10-11 12:41:17 | Open frame menu for 30 | Click (1250, 572) |
| 049 | 10-11 12:41:23 | Frame preset 30 | Click (1310, 751) |
| 050 | 10-11 12:41:28 | Open frame menu for 60 | Click (1250, 572) |
| 051 | 10-11 12:41:34 | Frame preset 60 | Click (1310, 783) |
| 052 | 10-11 12:41:39 | Custom frame size button | Click (1356, 572) |
| 053 | 10-11 12:41:46 | Custom frames select all | Ctrl+a |
| 054 | 10-11 12:41:52 | Custom frames 3 | Type: 3 |
| 055 | 10-11 12:41:57 | Custom frame OK mouse button | Click (886, 591) |
| 056 | 10-11 12:42:05 | Open frame menu restore 1 | Click (1250, 572) |
| 057 | 10-11 12:42:10 | Frame preset 1 restore | Click (1310, 591) |
| 058 | 10-11 12:42:25 | Custom MPV config enable | Click (1380, 781) |
| 059 | 10-11 12:42:34 | Reset custom MPV config button | Click (1350, 894) |
| 060 | 10-11 12:42:41 | MPV reset confirmation Cancel | Click (785, 602) |
| 061 | 10-11 12:42:46 | MPV revert button | Click (1350, 948) |
| 062 | 10-11 12:42:51 | MPV revert cancel Escape | Escape |
| 063 | 10-11 12:42:57 | MPV Edit File button | Click (1350, 840) |
| 064 | 10-11 12:43:27 | Hub Audio and Subtitles settings | Click (126, 247) |
| 065 | 10-11 12:43:38 | Preferred audio preset English | Click (350, 385) |
| 066 | 10-11 12:43:45 | Preferred audio preset French | Click (677, 385) |
| 067 | 10-11 12:43:50 | Preferred audio preset German | Click (841, 385) |
| 068 | 10-11 12:43:54 | Preferred audio preset Spanish | Click (1004, 385) |
| 069 | 10-11 12:44:00 | Preferred audio preset Italian | Click (1168, 385) |
| 070 | 10-11 12:44:05 | Preferred audio preset Default | Click (1332, 385) |
| 071 | 10-11 12:44:10 | Preferred audio preset Japanese restore | Click (514, 385) |
| 072 | 10-11 12:44:16 | Preferred subtitle preset Japanese | Click (514, 489) |
| 073 | 10-11 12:44:24 | Preferred subtitle preset French | Click (678, 489) |
| 074 | 10-11 12:44:28 | Preferred subtitle preset German | Click (841, 489) |
| 075 | 10-11 12:44:34 | Preferred subtitle preset Spanish | Click (1004, 489) |
| 076 | 10-11 12:44:39 | Preferred subtitle preset Italian | Click (1168, 489) |
| 077 | 10-11 12:44:44 | Preferred subtitle preset Off | Click (1332, 489) |
| 078 | 10-11 12:44:49 | Preferred subtitle preset English restore | Click (350, 489) |
| 079 | 10-11 12:44:56 | Subtitle size drag Medium to Extra large | Drag (715, 599) → (1396, 599) |
| 080 | 10-11 12:45:04 | Subtitle size Small endpoint 14px | Click (376, 599) |
| 081 | 10-11 12:45:09 | Subtitle size Large 26px | Click (1057, 599) |
| 082 | 10-11 12:45:15 | Subtitle size Medium 20px restore | Click (715, 599) |
| 083 | 10-11 12:45:21 | Subtitle color Yellow | Click (354, 729) |
| 084 | 10-11 12:45:26 | Subtitle color Green | Click (400, 729) |
| 085 | 10-11 12:45:31 | Subtitle color Cyan | Click (446, 729) |
| 086 | 10-11 12:45:36 | Subtitle color Orange | Click (492, 729) |
| 087 | 10-11 12:45:41 | Subtitle color Pink | Click (538, 729) |
| 088 | 10-11 12:45:47 | Subtitle color Lavender restore | Click (308, 729) |
| 089 | 10-11 12:46:00 | Subtitle background Translucent box | Click (697, 861) |
| 090 | 10-11 12:46:05 | Subtitle background Solid box | Click (984, 861) |
| 091 | 10-11 12:46:11 | Subtitle background Clean text | Click (1272, 861) |
| 092 | 10-11 12:46:17 | Subtitle background Outline restore | Click (412, 861) |
| 093 | 10-11 12:46:24 | Subtitle settings scroll to position controls | Wheel at (1290, 862); X=0, Y=526 |
| 094 | 10-11 12:46:30 | Subtitle position Top | Click (461, 940) |
| 095 | 10-11 12:46:35 | Subtitle position Elevated | Click (837, 940) |
| 096 | 10-11 12:46:41 | Subtitle position Bottom restore | Click (1220, 940) |
| 097 | 10-11 12:46:56 | Hub Keybindings settings category | Click (122, 294) |
| 098 | 10-11 12:47:35 | Keybinding Play/Pause capture button | Click (791, 260) |
| 099 | 10-11 12:47:44 | Keybinding conflict with M mute | m |
| 100 | 10-11 12:47:49 | Shortcut conflict cancel | Click (785, 602) |
| 101 | 10-11 12:48:02 | Reset shortcuts button | Click (1335, 147) |
| 102 | 10-11 12:48:09 | Reset shortcuts cancel Escape | Escape |
| 103 | 10-11 12:48:20 | Hub System and Storage settings category | Click (126, 341) |
| 104 | 10-11 12:48:43 | System Update Aperture button | Click (1340, 384) |
| 105 | 10-11 12:49:05 | Hover video previews disable | Click (1340, 452) |
| 106 | 10-11 12:49:30 | Hover video previews enable restore | Click (1340, 452) |
| 107 | 10-11 12:49:35 | Reduce motion enable | Click (1340, 520) |
| 108 | 10-11 12:49:42 | Reduce motion disable restore | Click (1340, 520) |
| 109 | 10-11 12:49:49 | Smart series detection enable | Click (1340, 588) |
| 110 | 10-11 12:49:57 | Smart series detection disable restore | Click (1340, 588) |
| 111 | 10-11 12:50:10 | Clear Cache button | Click (1340, 724) |
| 112 | 10-11 12:50:24 | Clear Watch History button | Click (1340, 792) |
| 113 | 10-11 12:50:37 | Clear History confirmation cancel | Click (777, 602) |
| 114 | 10-11 12:50:43 | Reset Preferences button | Click (1340, 860) |
| 115 | 10-11 12:50:48 | Reset Preferences cancel | Escape |
| 116 | 10-11 12:50:57 | Saved Data Open Data button | Click (1340, 656) |
| 117 | 10-11 12:51:26 | Open Data folder verified and closed | Explorer opened release/data; Alt+F4 closed only new folder window |
| 118 | 10-11 12:51:35 | Hub Media navigation | Click (63, 65) |
| 119 | 10-11 12:51:43 | Open video from Media double-click | Double click (506, 186) |
| 120 | 10-11 12:52:11 | Player Space before opening playback Settings | space |
| 121 | 10-11 12:52:19 | Player Settings gear open first time | Click (1332, 996) |
| 122 | 10-11 12:52:49 | Close player Settings with Escape | Escape |
| 123 | 10-11 12:56:05 | Space after opening and closing player Settings | space |
| 124 | 10-11 12:56:11 | Dismiss Settings and refocus video by clicking picture | Click (699, 399) |
| 125 | 10-11 12:56:16 | Space after clicking video to refocus | space |
| 126 | 10-11 12:56:23 | Open playback speed submenu | Click (1276, 690) |
| 127 | 10-11 12:56:30 | Speed flyout blank lower region hit test | Click (1275, 873) |
| 128 | 10-11 12:56:39 | Player speed preset 0.25x | Click (1208, 721) |
| 129 | 10-11 12:56:46 | Reopen Speed for next preset | Click (1288, 690) |
| 130 | 10-11 12:56:54 | Player speed preset 0.5x | Click (1294, 721) |
| 131 | 10-11 12:57:01 | Reopen Speed after0.5 | Click (1288, 690) |
| 132 | 10-11 12:57:06 | Player speed preset 0.75x | Click (1380, 721) |
| 133 | 10-11 12:57:09 | Reopen Speed after0.75 | Click (1288, 690) |
| 134 | 10-11 12:57:14 | Player speed preset1.0x | Click (1208, 756) |
| 135 | 10-11 12:57:20 | Reopen Speed after1.0 | Click (1288, 690) |
| 136 | 10-11 12:57:24 | Player speed preset1.25x | Click (1294, 756) |
| 137 | 10-11 12:57:29 | Reopen Speed after1.25 | Click (1288, 690) |
| 138 | 10-11 12:57:34 | Player speed preset1.5x | Click (1380, 756) |
| 139 | 10-11 12:57:41 | Reopen Speed after1.5 | Click (1288, 690) |
| 140 | 10-11 12:57:46 | Player speed preset1.75x | Click (1208, 791) |
| 141 | 10-11 12:57:51 | Reopen Speed after1.75 | Click (1288, 690) |
| 142 | 10-11 12:57:56 | Player speed preset2.0x | Click (1294, 791) |
| 143 | 10-11 12:58:01 | Reopen Speed after2.0 | Click (1288, 690) |
| 144 | 10-11 12:58:06 | Player speed preset3.0x | Click (1380, 791) |
| 145 | 10-11 12:58:11 | Open Speed slider test | Click (1288, 690) |
| 146 | 10-11 12:58:16 | Speed slider maximum to minimum | Drag (1410, 693) → (1178, 693) |
| 147 | 10-11 12:58:32 | Click speed slider center-left | Click (1255, 693) |
| 148 | 10-11 12:58:39 | Drag speed slider from interior thumb | Drag (1254, 693) → (1360, 693) |
| 149 | 10-11 12:58:58 | Restore player speed1x | Click (1208, 756) |
| 150 | 10-11 12:59:08 | Open player Audio settings | Click (1279, 648) |
| 151 | 10-11 12:59:14 | Select English audio track | Click (1286, 665) |
| 152 | 10-11 12:59:19 | Reopen Audio track selection | Click (1286, 648) |
| 153 | 10-11 12:59:24 | Restore Japanese audio track | Click (1286, 701) |
| 154 | 10-11 12:59:28 | Open player subtitle settings | Click (1278, 606) |
| 155 | 10-11 12:59:34 | Select subtitles Off | Click (1278, 665) |
| 156 | 10-11 12:59:39 | Reopen subtitle tracks | Click (1278, 606) |
| 157 | 10-11 12:59:43 | Select English(en) subtitle track | Click (1278, 701) |
| 158 | 10-11 12:59:48 | Reopen subtitle tracks again | Click (1278, 606) |
| 159 | 10-11 12:59:53 | Restore English subtitle track | Click (1278, 737) |
| 160 | 10-11 12:59:58 | Open aspect ratio settings | Click (1278, 732) |
| 161 | 10-11 13:00:03 | Aspect16:9 preset | Click (1278, 701) |
| 162 | 10-11 13:00:09 | Reopen Aspect choices | Click (1278, 732) |
| 163 | 10-11 13:00:14 | Aspect4:3 preset | Click (1278, 737) |
| 164 | 10-11 13:00:20 | Reopen Aspect after4:3 | Click (1278, 732) |
| 165 | 10-11 13:00:25 | Aspect21:9 preset | Click (1278, 773) |
| 166 | 10-11 13:00:31 | Reopen Aspect after21:9 | Click (1278, 732) |
| 167 | 10-11 13:00:37 | Aspect Stretch to Fill preset | Click (1278, 809) |
| 168 | 10-11 13:00:42 | Reopen Aspect afterfill | Click (1278, 732) |
| 169 | 10-11 13:00:49 | Aspect Zoom/Crop preset | Click (1278, 845) |
| 170 | 10-11 13:00:55 | Reopen Aspect to restore | Click (1278, 732) |
| 171 | 10-11 13:01:01 | Aspect Auto restore | Click (1278, 665) |
| 172 | 10-11 13:01:07 | Player Loop toggle on | Click (1408, 816) |
| 173 | 10-11 13:01:15 | Player Loop row toggle off restore | Click (1209, 816) |
| 174 | 10-11 13:01:20 | Player Autoplay off | Click (1408, 858) |
| 175 | 10-11 13:01:26 | Player Autoplay on restore via row | Click (1209, 858) |
| 176 | 10-11 13:01:32 | Private Session on | Click (1408, 900) |
| 177 | 10-11 13:01:39 | Private Session off restore via row | Click (1209, 900) |
| 178 | 10-11 13:01:44 | Open player Queue flyout | Click (1278, 774) |
| 179 | 10-11 13:01:49 | Queue submenu Back click | Click (1175, 623) |
| 180 | 10-11 13:01:55 | Player flyout Tab navigation | Tab |
| 181 | 10-11 13:02:00 | Flyout Down navigation | Down |
| 182 | 10-11 13:02:08 | Flyout Enter activates focused option | Return |
| 183 | 10-11 13:02:15 | Flyout Left should return main menu | Left |
| 184 | 10-11 13:02:21 | Flyout Shift Tab backwards | Shift+Tab |
| 185 | 10-11 13:02:27 | Flyout Up backwards | Up |
| 186 | 10-11 13:02:39 | Close flyout for keyboard shortcuts | Escape |
| 187 | 10-11 13:02:45 | Player Right seek+5 seconds | Right |
| 188 | 10-11 13:02:50 | Player Left seek-5 seconds | Left |
| 189 | 10-11 13:02:55 | Player L seek+10 seconds | l |
| 190 | 10-11 13:03:02 | Player J seek-10 seconds | j |
| 191 | 10-11 13:03:07 | Player volume down shortcut | Down |
| 192 | 10-11 13:03:12 | Player volume up restore shortcut | Up |
| 193 | 10-11 13:03:17 | Player M mute shortcut | m |
| 194 | 10-11 13:03:24 | Player Mute button unmute | Click (125, 996) |
| 195 | 10-11 13:03:29 | Drag player volume to zero | Drag (207, 996) → (147, 996) |
| 196 | 10-11 13:03:35 | Volume slider middle50 percent | Click (177, 996) |
| 197 | 10-11 13:03:40 | Restore volume100 percent | Click (207, 996) |
| 198 | 10-11 13:03:47 | Play button click | Click (39, 996) |
| 199 | 10-11 13:03:53 | Space after explicit Play button focus | space |
| 200 | 10-11 13:04:01 | Close reopened Settings during playback | Escape |
| 201 | 10-11 13:04:07 | Pause toolbar button | Click (39, 996) |
| 202 | 10-11 13:04:14 | S subtitle toggle shortcut | s |
| 203 | 10-11 13:04:20 | CC button cycle subtitles backon | Click (1290, 996) |
| 204 | 10-11 13:04:26 | CC cycle secondsubtitle restore | Click (1290, 996) |
| 205 | 10-11 13:04:33 | A audio cycle shortcut | a |
| 206 | 10-11 13:04:38 | A restore Japanese audio | a |
| 207 | 10-11 13:04:46 | Default speed-down[ shortcut conflict test | bracketleft |
| 208 | 10-11 13:04:52 | Default speed-up] shortcut conflict test | bracketright |
| 209 | 10-11 13:04:59 | Seek to create AB loop endpoint | Right |
| 210 | 10-11 13:05:05 | AB end with] overridesSpeedUp | bracketright |
| 211 | 10-11 13:05:12 | Clear AB repeat via backslash | backslash |
| 212 | 10-11 13:05:20 | Reset speed Backspace shortcut | BackSpace |
| 213 | 10-11 13:05:25 | Next frame shortcut | period |
| 214 | 10-11 13:05:31 | Previous frame shortcut | comma |
| 215 | 10-11 13:05:37 | Audio delay+ shortcut | Ctrl+Right |
| 216 | 10-11 13:05:44 | Audio delay- shortcut | Ctrl+Left |
| 217 | 10-11 13:05:50 | Audio delay reset shortcut | Ctrl+BackSpace |
| 218 | 10-11 13:05:56 | Subtitle delay+ shortcut | Ctrl+Shift+Right |
| 219 | 10-11 13:06:01 | Subtitle delay- shortcut | Ctrl+Shift+Left |
| 220 | 10-11 13:06:09 | Subtitle delay reset shortcut | Ctrl+Shift+BackSpace |
| 221 | 10-11 13:06:17 | F fullscreen shortcut | f |
| 222 | 10-11 13:06:23 | Exit fullscreen toolbar button | Click (1882, 1052) |
| 223 | 10-11 13:06:29 | PiP shortcut | p |
| 224 | 10-11 13:06:40 | PiP unpin toggle | Click (581, 27) |
| 225 | 10-11 13:06:47 | PiP pin restore | Click (581, 27) |
| 226 | 10-11 13:06:52 | PiP Volume open | Click (549, 27) |
| 227 | 10-11 13:06:58 | PiP volume drag min | Drag (549, 60) → (549, 123) |
| 228 | 10-11 13:07:06 | PiP volume slider minimum | Click (549, 133) |
| 229 | 10-11 13:07:13 | PiP volume maximum restore | Click (549, 58) |
| 230 | 10-11 13:07:19 | Close PiP volume popover | Click (549, 27) |
| 231 | 10-11 13:07:25 | PiP Play button | Click (60, 330) |
| 232 | 10-11 13:07:33 | PiP Space after playerSettings opened | space |
| 233 | 10-11 13:07:40 | Disabled PiP Previous hit test | Click (28, 330) |
| 234 | 10-11 13:07:45 | Disabled PiP Next hit test | Click (92, 330) |
| 235 | 10-11 13:07:52 | PiP video surface window drag | Drag (310, 120) → (390, 175) |
| 236 | 10-11 13:08:03 | Pause PiP after surface drag | space |
| 237 | 10-11 13:08:12 | PiP surface drag up-left | Drag (390, 170) → (230, 90) |
| 238 | 10-11 13:08:22 | Pause PiP aftersecond drag | space |
| 239 | 10-11 13:08:30 | PiP Restore Window button | Click (27, 27) |
| 240 | 10-11 13:08:35 | Next chapter shortcut | Page_Down |
| 241 | 10-11 13:08:41 | Previous chapter shortcut | Shift+p |
| 242 | 10-11 13:08:47 | Next video shortcut singlelibrary | n |
| 243 | 10-11 13:08:57 | Previous video shortcut singlelibrary | Page_Up |
| 244 | 10-11 13:09:03 | Restart video shortcut | r |
| 245 | 10-11 13:09:09 | Pause restartedvideo | Click (39, 996) |
| 246 | 10-11 13:09:15 | Cycle aspect X shortcut | x |
| 247 | 10-11 13:09:23 | Diagnostics overlay shortcut | i |
| 248 | 10-11 13:09:33 | Screenshot shortcut | Ctrl+s |
| 249 | 10-11 13:09:53 | Diagnostics off restore | i |
| 250 | 10-11 13:10:02 | Private Session custom0shortcut | 0 |
| 251 | 10-11 13:10:09 | Private Session shortcut restore | 0 |
| 252 | 10-11 13:10:52 | Timeline midpoint pointer seek | Click (728, 964) |
| 253 | 10-11 13:13:54 | Timeline drag from midpoint to three-quarter | Drag (728, 964) → (1090, 964) |
| 254 | 10-11 13:13:59 | Timeline left endpoint seeks zero | Click (22, 964) |
| 255 | 10-11 13:14:04 | Timeline right endpoint seeks duration | Click (1433, 964) |
| 256 | 10-11 13:14:10 | Playback time label input response | Click (270, 996) |
| 257 | 10-11 13:14:16 | Player context menu opens at visible cursor | Right click (600, 450) |
| 258 | 10-11 13:14:22 | Open player More Options | Click (704, 839) |
| 259 | 10-11 13:14:29 | Open content speed named presets | Click (716, 636) |
| 260 | 10-11 13:14:36 | Content preset Audiobook 1.20x | Click (720, 502) |
| 261 | 10-11 13:14:47 | Reopen player context for content presets | Right click (600, 450) |
| 262 | 10-11 13:14:52 | More Options for next speed preset | Click (704, 839) |
| 263 | 10-11 13:14:57 | Content speed menu for Lecture | Click (716, 636) |
| 264 | 10-11 13:15:03 | Content preset Lecture 1.25x | Click (720, 534) |
| 265 | 10-11 13:15:11 | Player context for Podcast preset | Right click (600, 450) |
| 266 | 10-11 13:15:17 | More Options for Podcast | Click (704, 839) |
| 267 | 10-11 13:15:23 | Content speed menu for Podcast selection | Click (716, 636) |
| 268 | 10-11 13:15:34 | Content preset Podcast 1.50x | Click (720, 566) |
| 269 | 10-11 13:15:42 | Player context for Review preset | Right click (600, 450) |
| 270 | 10-11 13:15:49 | More Options for Review | Click (704, 839) |
| 271 | 10-11 13:15:57 | Content speed menu for Review selection | Click (716, 636) |
| 272 | 10-11 13:16:03 | Content preset Review Skim 2.00x | Click (720, 598) |
| 273 | 10-11 13:16:09 | Player context for Cinema preset | Right click (600, 450) |
| 274 | 10-11 13:16:17 | More Options for Cinema | Click (704, 839) |
| 275 | 10-11 13:16:24 | Content speed menu for Cinema selection | Click (716, 636) |
| 276 | 10-11 13:16:31 | Content preset Cinema TV 1.00x restored | Click (720, 470) |
| 277 | 10-11 13:16:37 | Player context for Sleep Timer | Right click (600, 450) |
| 278 | 10-11 13:16:44 | More Options for Sleep Timer | Click (704, 839) |
| 279 | 10-11 13:16:50 | Open Sleep Timer presets | Click (714, 668) |
| 280 | 10-11 13:16:57 | Sleep Timer 15 minutes | Click (720, 470) |
| 281 | 10-11 13:17:06 | Player context for 30-minute timer | Right click (600, 450) |
| 282 | 10-11 13:17:13 | More Options for 30-minute timer | Click (704, 839) |
| 283 | 10-11 13:17:23 | Sleep Timer menu for 30 minutes | Click (714, 668) |
| 284 | 10-11 13:17:33 | Sleep Timer 30 minutes | Click (720, 502) |
| 285 | 10-11 13:17:44 | Player context for 60-minute timer | Right click (600, 450) |
| 286 | 10-11 13:17:52 | More Options for 60-minute timer | Click (704, 839) |
| 287 | 10-11 13:17:58 | Sleep Timer menu for 60 minutes | Click (714, 668) |
| 288 | 10-11 13:18:05 | Sleep Timer 60 minutes | Click (720, 534) |
| 289 | 10-11 13:18:14 | Player context for 90-minute timer | Right click (600, 450) |
| 290 | 10-11 13:18:20 | More Options for 90-minute timer | Click (704, 839) |
| 291 | 10-11 13:18:28 | Sleep Timer menu for 90 minutes | Click (714, 668) |
| 292 | 10-11 13:18:36 | Sleep Timer 90 minutes | Click (720, 566) |
| 293 | 10-11 13:18:52 | Player context for Stop After Current | Right click (600, 450) |
| 294 | 10-11 13:19:01 | More Options for Stop After Current | Click (704, 839) |
| 295 | 10-11 13:19:10 | Sleep Timer menu for stop after current | Click (714, 668) |
| 296 | 10-11 13:19:16 | Sleep Stop After Current enabled | Click (720, 598) |
| 297 | 10-11 13:19:22 | Player context for cancel timer | Right click (600, 450) |
| 298 | 10-11 13:19:30 | More Options for cancel timer | Click (704, 839) |
| 299 | 10-11 13:19:40 | Sleep Timer menu to cancel | Click (714, 668) |
| 300 | 10-11 13:19:49 | Cancel Sleep Timer removes timer and stop-after-current | Click (720, 630) |
| 301 | 10-11 13:19:57 | Player context for audio delay | Right click (600, 450) |
| 302 | 10-11 13:20:06 | More Options for Audio Delay | Click (704, 839) |
| 303 | 10-11 13:20:15 | Open Audio Delay context submenu | Click (714, 700) |
| 304 | 10-11 13:20:21 | Audio Delay menu Increase +50ms | Click (696, 508) |
| 305 | 10-11 13:20:32 | Audio Delay menu Decrease -50ms | Click (696, 540) |
| 306 | 10-11 13:20:44 | Audio Delay menu negative -50ms | Click (696, 540) |
| 307 | 10-11 13:20:56 | Audio Delay menu Reset restores zero | Click (696, 572) |
| 308 | 10-11 13:21:02 | Audio Delay Back — returned the root Player context menu | Click (696, 610) |
| 309 | 10-11 13:21:11 | More Options for Audio Balance | Click (704, 839) |
| 310 | 10-11 13:21:18 | Open Audio Balance presets | Click (714, 732) |
| 311 | 10-11 13:21:28 | Audio Balance shift left 10 percent | Click (700, 508) |
| 312 | 10-11 13:21:37 | Audio Balance shift right 10 percent | Click (700, 540) |
| 313 | 10-11 13:21:49 | Audio Balance Full Left 100 percent | Click (700, 578) |
| 314 | 10-11 13:22:02 | Audio Balance Full Right 100 percent | Click (700, 642) |
| 315 | 10-11 13:22:13 | Audio Balance Center reset restored | Click (700, 610) |
| 316 | 10-11 13:22:21 | Audio Balance Back returns root Player menu | Click (700, 680) |
| 317 | 10-11 13:22:32 | More Options for Channel Mapping | Click (704, 839) |
| 318 | 10-11 13:22:39 | Open Audio Channel Mapping presets | Click (714, 764) |
| 319 | 10-11 13:22:52 | Channel Mapping Stereo Direct | Click (706, 540) |
| 320 | 10-11 13:23:03 | Channel Mapping Mono Left | Click (706, 572) |
| 321 | 10-11 13:23:09 | Channel Mapping Mono Right | Click (706, 604) |
| 322 | 10-11 13:23:16 | Channel Mapping Swap Left Right | Click (706, 636) |
| 323 | 10-11 13:23:26 | Channel Mapping Auto Default | Click (706, 508) |
| 324 | 10-11 13:23:38 | Channel Mapping Reset to Auto | Click (706, 674) |
| 325 | 10-11 13:23:44 | Channel Mapping Back returns root | Click (706, 712) |
| 326 | 10-11 13:23:54 | More Options for Subtitle Delay | Click (704, 839) |
| 327 | 10-11 13:24:01 | Open Subtitle Delay context menu | Click (714, 796) |
| 328 | 10-11 13:24:12 | Subtitle Delay menu Increase +100ms | Click (696, 508) |
| 329 | 10-11 13:24:23 | Subtitle Delay menu Decrease -100ms | Click (696, 540) |
| 330 | 10-11 13:24:31 | Subtitle Delay Reset zero | Click (696, 572) |
| 331 | 10-11 13:24:45 | Subtitle Delay Back returns root | Click (696, 610) |
| 332 | 10-11 13:24:57 | More Options for Video Adjustments | Click (704, 839) |
| 333 | 10-11 13:25:11 | Open Video Adjustments | Click (710, 860) |
| 334 | 10-11 13:25:23 | Video Rotation 90 degrees | Click (709, 508) |
| 335 | 10-11 13:25:48 | Video Rotation 180 degrees | Click (709, 508) |
| 336 | 10-11 13:26:00 | Video Rotation 270 degrees | Click (709, 508) |
| 337 | 10-11 13:26:11 | Video Rotation returns zero | Click (709, 508) |
| 338 | 10-11 13:26:23 | Open Brightness adjustment | Click (709, 540) |
| 339 | 10-11 13:26:30 | Brightness Increase +5 | Click (696, 508) |
| 340 | 10-11 13:26:44 | Brightness Decrease -5 | Click (696, 540) |
| 341 | 10-11 13:26:55 | Brightness Reset zero | Click (696, 572) |
| 342 | 10-11 13:27:03 | Brightness Back returns Video Adjustments | Click (696, 610) |
| 343 | 10-11 13:27:11 | Open Contrast adjustment | Click (709, 572) |
| 344 | 10-11 13:27:27 | Contrast Increase +5 | Click (696, 508) |
| 345 | 10-11 13:27:39 | Contrast Decrease -5 | Click (696, 540) |
| 346 | 10-11 13:27:47 | Contrast Reset zero | Click (696, 572) |
| 347 | 10-11 13:27:54 | Contrast Back | Click (696, 610) |
| 348 | 10-12 12:15:22 | Hub media filter shortcut Ctrl F | Ctrl+f |
| 349 | 10-12 12:15:38 | Media filter no results | Type: no-such-audit-item |
| 350 | 10-12 12:15:51 | Clear Search empty state button | Click (720, 625) |
| 351 | 10-12 12:16:07 | Open Media sort presets | Click (1172, 131) |
| 352 | 10-12 12:16:18 | Media sort Title preset | Click (1190, 172) |
| 353 | 10-12 12:16:35 | Reopen Media sort for Date Added | Click (1172, 131) |
| 354 | 10-12 12:16:46 | Media sort Date Added preset | Click (1190, 204) |
| 355 | 10-12 12:16:59 | Reopen Media sort for Last Played | Click (1172, 131) |
| 356 | 10-12 12:17:14 | Media sort Last Played preset | Click (1190, 236) |
| 357 | 10-12 12:17:26 | Reopen Media sort for Duration | Click (1172, 131) |
| 358 | 10-12 12:17:39 | Media sort Duration preset | Click (1190, 268) |
| 359 | 10-12 12:17:50 | Reopen Media sort for Descending | Click (1172, 131) |
| 360 | 10-12 12:17:59 | Media sort Descending | Click (1190, 338) |
| 361 | 10-12 12:18:12 | Reopen Media sort for Ascending | Click (1172, 131) |
| 362 | 10-12 12:18:24 | Media sort Ascending | Click (1190, 306) |
| 363 | 10-12 12:18:40 | Reopen sort for restoration | Click (1172, 131) |
| 364 | 10-12 12:18:54 | Restore Media Title ascending | Click (1190, 172) |
| 365 | 10-12 12:19:06 | Media Folders grouping | Click (1390, 131) |
| 366 | 10-12 12:19:19 | Collapse Media folder chevron | Click (45, 175) |
| 367 | 10-12 12:19:29 | Expand Media folder chevron | Click (45, 175) |
| 368 | 10-12 12:19:39 | Folder context menu | Right click (130, 175) |
| 369 | 10-12 12:19:54 | Folder Rediscover Reconnect missing items | Click (235, 259) |
| 370 | 10-12 12:20:20 | Folder context reopen for remove confirmation | Right click (130, 175) |
| 371 | 10-12 12:20:37 | Folder remove dialog open | Click (235, 296) |
| 372 | 10-12 12:20:54 | Undo folder removal restore original library | Ctrl+z |
| 373 | 10-12 12:22:19 | Empty library Open File restore original entry | Click (655, 625) |
| 374 | 10-12 12:23:05 | Focus native file name field | Click (450, 955); native picker image |
| 375 | 10-12 12:23:20 | Restore original file path | Type: D:\Movies\DanMachi\[Kayoanime] DanMachi - S05E01.mkv |
| 376 | 10-12 12:23:34 | Native picker Open restore original file | Click (1745, 986); native picker image |
| 377 | 10-12 12:23:48 | Space fresh player before Settings after relaunch | space |
| 378 | 10-12 12:24:43 | Restore initial resume position approximately 5 minutes 5 seconds | Click (324, 964) |
| 379 | 10-12 12:24:56 | Player Back button return Hub restore original | Click (43, 65) |
| 380 | 10-12 12:25:08 | Folder context Open Explorer test | Right click (130, 175) |
| 381 | 10-12 12:25:18 | Open folder in Explorer | Click (239, 227) |
| 382 | 10-12 12:26:12 | Media item context in grouped view | Right click (300, 220) |
| 383 | 10-12 12:26:26 | Media mark watched | Click (400, 335) |
| 384 | 10-12 12:26:37 | Media watched-state menu reopen | Right click (300, 220) |
| 385 | 10-12 12:27:51 | Dismiss media context with Escape | Escape |
| 386 | 10-12 12:28:07 | Refresh library keyboard response | F5 |
| 387 | 10-12 12:28:25 | Check watched-state menu after F5 | Right click (300, 220) |
| 388 | 10-12 12:28:50 | Media background context selected one item | Right click (700, 500) |
| 389 | 10-12 12:29:18 | Background mark selected unwatched | Click (808, 616) |
| 390 | 10-12 12:29:38 | Background reopen for Refresh button | Right click (700, 500) |
| 391 | 10-12 12:29:53 | Background Refresh button | Click (808, 820) |
| 392 | 10-12 12:30:09 | Restore Standalone grouping | Click (1285, 131) |
| 393 | 10-12 12:30:37 | Standalone media context controls | Right click (300, 184) |
| 394 | 10-12 12:30:51 | Single media Remove from Library confirmation | Click (400, 382) |
| 395 | 10-12 12:31:45 | Cancel single media removal | Click (784, 601) |
| 396 | 10-12 12:32:03 | Hub Ctrl Right switches Media to Playlists | Ctrl+Right |
| 397 | 10-12 12:32:16 | Add Playlist plus button | Click (252, 143) |
| 398 | 10-12 12:32:31 | Select new playlist default name | Ctrl+a |
| 399 | 10-12 12:32:49 | New temporary playlist name | Type: UI Audit 2026-10-12 |
| 400 | 10-12 12:33:06 | Create audit playlist with OK button | Click (887, 592) |
| 401 | 10-12 12:33:31 | Empty audit playlist Add Files | Click (854, 380) |
| 402 | 10-12 12:33:54 | Focus playlist Add Files filename | Click (450, 955); native picker image |
| 403 | 10-12 12:34:15 | Add three observed media files to audit playlist | Type: "[Kayoanime] DanMachi - S05E01.mkv" "[Kayoanime] DanMachi - S05E02.mkv" "[Kayoanime] DanMachi - S05E03.mkv" |
| 404 | 10-12 12:34:34 | Add selected three media via Enter | Return |
| 405 | 10-12 12:34:53 | Playlist Grid View button | Click (1394, 339) |
| 406 | 10-12 12:35:07 | Playlist filter Ctrl F | Ctrl+f |
| 407 | 10-12 12:35:24 | Filter playlist exact episode | Type: S05E02 |
| 408 | 10-12 12:35:40 | Playlist filter Escape clears query | Escape |
| 409 | 10-12 12:35:58 | Playlist List View button | Click (1362, 339) |
| 410 | 10-12 12:36:18 | Playlist reorder first episode after third via handle | Drag (307, 398) → (307, 562) |
| 411 | 10-12 12:36:33 | Playlist reorder restore first episode at beginning | Drag (307, 534) → (307, 368) |
| 412 | 10-12 12:36:53 | Playlist Down selects second episode | Down |
| 413 | 10-12 12:37:18 | Playlist Shift Down range selection | Shift+Down |
| 414 | 10-12 12:37:36 | Playlist Home selects first | Home |
| 415 | 10-12 12:39:00 | Playlist select all keyboard | Ctrl+a |
| 416 | 10-12 12:39:21 | Playlist multi-item context | Right click (600, 534) |
| 417 | 10-12 12:39:40 | Playlist mark three watched | Click (702, 585) |
| 418 | 10-12 12:39:57 | Playlist context reopen to mark all unwatched | Right click (600, 534) |
| 419 | 10-12 12:40:11 | Playlist mark three unwatched restore | Click (702, 617) |
| 420 | 10-12 12:40:33 | Playlist multi selection removal menu | Right click (600, 534) |
| 421 | 10-12 12:40:51 | Remove three entries from temporary audit playlist | Click (702, 655) |
| 422 | 10-12 12:41:11 | Undo multi-item playlist removal | Ctrl+z |
| 423 | 10-12 12:41:36 | Re-add temporary playlist fixture | Click (854, 380) |
| 424 | 10-12 12:42:16 | Focus native filename to restore fixture | Click (450, 955); native picker image |
| 425 | 10-12 12:42:35 | Restore temporary fixture three filenames | Type: "[Kayoanime] DanMachi - S05E01.mkv" "[Kayoanime] DanMachi - S05E02.mkv" "[Kayoanime] DanMachi - S05E03.mkv" |
| 426 | 10-12 12:42:54 | Restore audit playlist entries Enter | Return |
| 427 | 10-12 12:43:09 | Playlist Play All button starts three-item queue | Click (1317, 151) |
<!-- END PERSISTED STEP LOG -->
