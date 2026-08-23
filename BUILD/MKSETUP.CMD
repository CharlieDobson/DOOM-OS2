@echo off
rem --------------------------------------------------------------------------
rem  MKSETUP.CMD -- build OS2SETUP.EXE, the settings program for DOOM for OS/2.
rem
rem  Runs on Windows NT or later with Open Watcom 1.9 installed, cross building
rem  for OS/2 2.x and Warp, exactly as MKOS2.CMD does for the game itself.
rem
rem  NOT called SETUP.EXE.  The DOS game's own SETUP.EXE lives in the same
rem  directory as the WAD and still works for the DOS build; a port has no
rem  business overwriting it.
rem
rem  This is a pure Presentation Manager program -- linked "sys os2v2_pm", not
rem  the plain os2v2 the game uses.  The game is linked as a text mode program
rem  and morphs into a PM one at run time so that its startup text has a
rem  session to appear in; this has nothing to say and no reason to open one.
rem --------------------------------------------------------------------------

if "%WATCOM%"=="" goto nowatcom

rem  -bt=os2   Target OS/2, 32 bit.
rem  -bm       Multithreaded run-time.  PM programs are, whether they say so
rem            or not.
rem  -5r       486 and later, register calling convention.  Matches the game.
rem  -zq       Quiet.
rem  -w1       Enough warnings to be useful without burying them.
rem
rem  NOTE: none of -ei, -j or the alias flags matter here.  This program shares
rem            no structure with DOOM and lays nothing over a file format --
rem            it reads DEFAULT.CFG as text, a line at a time.  The traps
rem            documented in MKOS2.CMD are traps for the engine, not for this.

set SFLAGS=-bt=os2 -bm -5r -zq -w1 -e25 -i=%WATCOM%\H -i=%WATCOM%\H\OS2

echo Compiling OS2SETUP...

wcc386 ..\os2setup.c %SFLAGS%
if errorlevel 1 goto failed
if not exist os2setup.obj goto failed

echo Linking...

wlink >nul: 2>&1 ^
  system os2v2_pm ^
  name OS2SETUP.EXE ^
  option quiet option map option stack=65536 ^
  option description 'DOOM for OS/2 - Setup' ^
  file os2setup.obj

if errorlevel 1 goto failed
if not exist OS2SETUP.EXE goto failed

del os2setup.obj >nul 2>&1

echo.
echo Built BUILD\OS2SETUP.EXE  (OS/2 2.x / Warp, 32-bit Presentation Manager).
echo.
echo Put it in the same directory as DOOM.EXE and DEFAULT.CFG and run it from
echo the desktop.  It edits DEFAULT.CFG in place, keeping every setting it
echo does not understand -- including the DOS game's, if they are still there.
echo.
goto end

:failed
echo.
echo *** BUILD FAILED ***
echo.
goto end

:nowatcom
echo.
echo WATCOM is not set.  Open Watcom 1.9 must be installed and its environment
echo set up before running this.
echo.

:end
