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
rem    2. wlink   sys os2v2 -> DOOM.EXE  (LX, 1 MB stack)
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
rem  -w1       DOOM is from 1997 and was written for gcc.  At -w4 the
rem            unreferenced rcsid[] string at the top of all sixty files, and
rem            a good deal else that is deliberate, buries anything worth
rem            reading.
rem --------------------------------------------------------------------------
set CFLAGS=-bt=os2 -bm -dNORMALUNIX -otexan -5r -zq -w1 -e25 -i=.. -i=%WATCOM%\H -i=%WATCOM%\H\OS2

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
for %%F in (i_main i_system i_video i_sound i_net) do (
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
rem     "sys os2v2", not "os2v2_pm".
rem
rem     This produces a text-mode (VIO) executable, and I_InitGraphics turns
rem     the running process into a Presentation Manager one -- see
rem     I_OS2_MorphToPM in I_SYSTEM.C.  That is what lets DOOM print its
rem     startup banner to the OS/2 window it was launched from and still open
rem     a PM window a moment later.
rem
rem     Linking "sys os2v2_pm" instead also works and needs no source change,
rem     but everything printed before the window opens is lost, including the
rem     message when a WAD cannot be found.  I_Error puts that in a message
rem     box as well, for exactly this reason.
rem
rem     Nothing is imported beyond the base system: DIVE, MMPM/2 and TCP/IP
rem     are all loaded by name at run time, so this DOOM.EXE loads on a plain
rem     OS/2 2.0 with no multimedia and no networking, and simply does
rem     without them.
rem --------------------------------------------------------------------------
echo Linking...
wlink sys os2v2 name DOOM.EXE ^
  option quiet option map option stack=%STACK% ^
  option description 'DOOM for OS/2' ^
  file i_main.obj,i_system.obj,i_video.obj,i_sound.obj,i_net.obj, ^
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

del *.obj >nul 2>&1

echo.
echo Built BUILD\DOOM.EXE  (OS/2 2.x / Warp, 32-bit).
echo.
echo Put it beside a DOOM WAD, or set DOOMWADDIR to point at one, and run it
echo from an OS/2 window.  See README.OS2.
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
