@echo off
setlocal
rem ==========================================================================
rem  Build DOOM for OS/2 2.x / Warp (DOOM.EXE, 32-bit) from THIS
rem  subdirectory.  Sources are one level up.  Building from here keeps the
rem  object files out of the source tree.
rem
rem  Cross-built: Open Watcom's NT-hosted compiler emits OS/2 LX objects, so
rem  this runs on the Windows box and produces an executable for OS/2.  It
rem  cannot be run here.
rem
rem  Steps:
rem    1. wcc386  -bt=os2   -> one .OBJ per source
rem    2. wlink   sys os2v2_pm -> DOOM.EXE  (LX, PM, 1 MB stack)
rem
rem  There are no resources to bind: the port creates its window entirely in
rem  code and has no icon of its own.  See README.OS2 for how to add one.
rem ==========================================================================
rem  Watcom location.  An existing %WATCOM% is honoured -- a normal Open
rem  Watcom install sets it -- so the fallback below only matters when the
rem  variable is unset.  A path containing spaces will not work: Watcom's
rem  -i= option takes no quoting.
if not defined WATCOM set WATCOM=C:\WATCOM

if not exist "%WATCOM%\binnt\wlink.exe" goto nowatcom

set INCLUDE=%WATCOM%\H;%WATCOM%\H\OS2
set PATH=%WATCOM%\BINNT;%WATCOM%\BINW;%PATH%

rem --------------------------------------------------------------------------
rem  Compiler flags.
rem
rem  -bt=os2   target OS/2 32-bit.
rem
rem  -bm       MULTITHREADED runtime.  Not optional.  MMPM/2 calls
rem            I_DartEvent back on a thread of its own to fill the next sound
rem            buffer, so there are genuinely two threads in this process.
rem            Without -bm the program still links, but errno and the heap
rem            are not thread safe and the corruption is intermittent --
rem            every module must have it, engine modules included.
rem
rem  -dNORMALUNIX
rem            DOOM's own switch for "a machine with POSIX headers and a
rem            desktop keyboard", and OS/2 with Open Watcom is exactly that:
rem            unistd.h, fcntl.h, sys/stat.h, malloc.h and alloca.h are all
rem            present.  It also selects the arrow-key and shift/ctrl/alt
rem            default bindings in M_MISC.C, which are the ones this port's
rem            keyboard handler produces.  Where OS/2 needs to differ, the
rem            sources test __OS2__, which the compiler defines by itself.
rem
rem  -5r       Pentium instruction scheduling, register calling convention.
rem            Still 386 compatible code -- it changes the order of the
rem            instructions, not which ones -- so a Warp machine with a 486
rem            in it runs this perfectly well.
rem
rem  -ei       Enums are int sized, as gcc makes them.
rem
rem            NOT optional, and the reason is worth writing down.  Open
rem            Watcom sizes an enum as the smallest type that will hold it,
rem            so DOOM's "typedef enum {false,true} boolean" comes out ONE
rem            byte.  Under gcc, which the released code was built with, it
rem            is four.  That matters because R_DATA.C lays maptexture_t
rem            straight over the TEXTURE1 lump, and maptexture_t has a
rem            boolean in the middle of it: without -ei every texture's width
rem            is read from the wrong offset and comes back zero, and no wall
rem            in the game draws properly.  Nothing warns; it just looks
rem            wrong.
rem
rem  -j        Plain char is signed, as gcc makes it.
rem
rem            NOT optional either, and for the same kind of reason.  Open
rem            Watcom makes an unqualified char UNSIGNED by default.  DOOM
rem            carries a tic's movement in ticcmd_t as "char forwardmove",
rem            and walking backwards is simply a negative one -- so unsigned,
rem            -25 arrives as 231 and pressing the back key runs the player
rem            forward at speed.  Strafing left goes the same way.  Nothing
rem            warns about this one either.
rem
rem            Two defaults now, -ei and -j, where Open Watcom differs from
rem            gcc and the difference is silent.  Anything this code lays
rem            over a file, a wire or a hardware structure is worth checking
rem            against the compiler it was originally built with.
rem
rem  NOTE: the flags are -otexn, NOT -otexan.  The "a" is "relax alias
rem            checking", which tells the compiler that pointers of different
rem            types never refer to the same storage.  DOOM breaks that rule
rem            constantly and deliberately -- the column drawing, screens[0],
rem            the network packet swabbing -- so it is exactly the wrong
rem            promise to make here, and the symptom is subtly wrong rendering
rem            rather than a crash.
rem
rem  -w1       DOOM is from 1997 and was written for gcc.  At -w4 the
rem            unreferenced rcsid[] string at the top of all sixty files, and
rem            a good deal else that is deliberate, buries anything worth
rem            reading.
rem --------------------------------------------------------------------------
set CFLAGS=-bt=os2 -bm -dNORMALUNIX -otexn -5r -ei -j -zq -w1 -e25 -i=.. -i=%WATCOM%\H -i=%WATCOM%\H\OS2

rem  !! STACK !!  1 MB, not the default.
rem
rem  Two things want it.  R_RenderBSPNode recurses once per node down the
rem  level's BSP tree, and W_AddFile and R_InitTextures both take the lump
rem  directory on the stack with alloca -- sixteen bytes per lump, so a large
rem  PWAD can ask for a hundred kilobytes in one go.  OS/2 commits stack
rem  pages on demand, so reserving this much costs nothing until it is used.
set STACK=1048576

pushd "%~dp0"

del DOOM.EXE >nul 2>&1
del *.obj >nul 2>&1

rem --------------------------------------------------------------------------
rem  1. compile
rem
rem     The platform layer -- I_MAIN, I_SYSTEM, I_VIDEO, I_SOUND, I_NET -- is
rem     the OS/2 port.  Everything else is the engine, as id released it,
rem     apart from the handful of places noted in README.OS2.
rem --------------------------------------------------------------------------
echo Compiling the OS/2 platform layer...
for %%F in (i_main i_system i_video i_sound i_net d_os2iwd i_os2mus i_os2log i_os2load) do (
  wcc386 ..\%%F.c %CFLAGS%
  if errorlevel 1 goto failed
  if not exist %%F.obj goto failed
)

echo Compiling the engine...
for %%F in (
  doomdef doomstat dstrings tables f_finale f_wipe d_main d_net d_items
  g_game m_menu m_misc m_argv m_bbox m_fixed m_swap m_cheat m_random
  am_map p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl
  p_plats p_pspr p_setup p_sight p_spec p_switch p_mobj p_telept p_tick
  p_saveg p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things
  w_wad wi_stuff v_video st_lib st_stuff hu_stuff hu_lib s_sound z_zone
  info sounds
) do (
  wcc386 ..\%%F.c %CFLAGS%
  if errorlevel 1 goto failed
  if not exist %%F.obj goto failed
)

rem --------------------------------------------------------------------------
rem  2. link
rem
rem     "sys os2v2_pm", a real Presentation Manager application.
rem
rem     It was not always.  For most of this port's life it was linked "sys
rem     os2v2" -- a text mode program that turned itself into a PM one at run
rem     time (I_OS2_MorphToPM, still in I_SYSTEM.C and still called, harmlessly
rem     returning at once now that the process is already type 3).  That was
rem     done so DOOM's startup commentary had a session to appear in.
rem
rem     Two things replaced that session and are better than it was.  DOOM.LOG
rem     keeps every line, so a run that went wrong can be read afterwards
rem     rather than only watched at the time; and the loading window shows a
rem     player what is happening in words meant for them.  Neither needs a
rem     command prompt behind the game, and the VIO session brought problems
rem     of its own -- it competed for the keyboard focus, and it outlived the
rem     game whenever the process failed to exit cleanly.
rem
rem     I_Error still puts its message in a PM message box, which is what
rem     makes a missing WAD reportable with no session to report it to.
rem
rem     Going back is one word: os2v2_pm -> os2v2, no source change.
rem
rem     Nothing is imported beyond the base system: DIVE, MMPM/2 and TCP/IP
rem     are all loaded by name at run time, so this DOOM.EXE loads on a plain
rem     OS/2 2.0 with no multimedia and no networking, and simply does
rem     without them.
rem --------------------------------------------------------------------------
echo Linking...
wlink sys os2v2_pm name DOOM.EXE ^
  option quiet option map option stack=%STACK% ^
  option description 'DOOM for OS/2' ^
  file i_main.obj,i_system.obj,i_video.obj,i_sound.obj,i_net.obj, ^
       d_os2iwd.obj,i_os2mus.obj,i_os2log.obj,i_os2load.obj, ^
       doomdef.obj,doomstat.obj,dstrings.obj,tables.obj,f_finale.obj, ^
       f_wipe.obj,d_main.obj,d_net.obj,d_items.obj,g_game.obj,m_menu.obj, ^
       m_misc.obj,m_argv.obj,m_bbox.obj,m_fixed.obj,m_swap.obj, ^
       m_cheat.obj,m_random.obj,am_map.obj,p_ceilng.obj,p_doors.obj, ^
       p_enemy.obj,p_floor.obj,p_inter.obj,p_lights.obj,p_map.obj, ^
       p_maputl.obj,p_plats.obj,p_pspr.obj,p_setup.obj,p_sight.obj, ^
       p_spec.obj,p_switch.obj,p_mobj.obj,p_telept.obj,p_tick.obj, ^
       p_saveg.obj,p_user.obj,r_bsp.obj,r_data.obj,r_draw.obj,r_main.obj, ^
       r_plane.obj,r_segs.obj,r_sky.obj,r_things.obj,w_wad.obj, ^
       wi_stuff.obj,v_video.obj,st_lib.obj,st_stuff.obj,hu_stuff.obj, ^
       hu_lib.obj,s_sound.obj,z_zone.obj,info.obj,sounds.obj
if errorlevel 1 goto failed
if not exist DOOM.EXE goto failed

rem --------------------------------------------------------------------------
rem  3. resources (optional)
rem
rem     Only if there is an icon to bind.  RES\DOOM.OS2 must be a genuine
rem     OS/2 icon; see the comment at the top of RES\DOOM.RC for why a
rem     Windows .ICO will not do, and why nothing breaks if this step is
rem     skipped entirely.
rem --------------------------------------------------------------------------
if not exist ..\RES\DOOM.OS2 goto noresources

echo Compiling and binding resources...
wrc -bt=os2 -r -q -i=..\RES -i=%WATCOM%\H\OS2 ..\RES\DOOM.RC -fo=DOOM.res
if errorlevel 1 goto failed

wrc -bt=os2 -q DOOM.res DOOM.EXE
if errorlevel 1 goto failed
goto haveresources

:noresources
echo No RES\DOOM.OS2 icon - building without one (the default icon is used).

:haveresources

del *.obj >nul 2>&1

echo.
echo Built BUILD\DOOM.EXE  (OS/2 2.x / Warp, 32-bit).
echo.
echo Put it beside a DOOM WAD, or set DOOMWADDIR to point at one, and run it
echo from the desktop or an OS/2 window.  See README.OS2.
popd
endlocal
goto :eof

:nowatcom
echo.
echo *** Open Watcom was not found at "%WATCOM%".
echo ***
echo *** Set the WATCOM environment variable to your Open Watcom 1.9
echo *** installation directory, or edit the fallback near the top of
echo *** this script.
endlocal
exit /b 1

:failed
echo.
echo *** BUILD FAILED - see the wcc386/wlink errors above.
popd
endlocal
exit /b 1
