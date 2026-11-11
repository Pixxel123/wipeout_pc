#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepares the game image and starts gow3-probe (Windows and Linux; run.sh wraps it on Linux).

Environment variables (GOW3_GAME_DIR, GOW3_DATA_DIR, GOW3_PATCHES, GOW3_MODS_*,
GOW3_USER_DIR, ...). Extra arguments go to gow3-probe.
The in-game "Apply and restart" runs this script again through GOW3_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time

WINDOWS = sys.platform == 'win32'
EXE_SUFFIX = '.exe' if WINDOWS else ''
if WINDOWS:
    import ctypes

PORT = Path(__file__).resolve().parent


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    if not WINDOWS:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            except PermissionError:
                pass
            time.sleep(0.1)
        return
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    if not WINDOWS:
        return 0
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def parse_cpu_features(output):
    """Instruction sets `gow3-probe --cpu-features` reports missing ([] when complete)."""
    for line in output.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
    return []


def isa_rewrite_wanted(missing, env):
    """GOW3_ISA_REWRITE=1 forces the ISA rewrite table, 0 disables it; otherwise the CPU decides."""
    policy = env.get('GOW3_ISA_REWRITE')
    if policy in ('0', '1'):
        return policy == '1'
    return bool(missing)


def run_script(name, *args, capture=False, check=True):
    # Inside the packaged launcher executable (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    env = {**os.environ, 'PYTHONIOENCODING': 'utf-8'}
    if capture:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                encoding='utf-8', env=env, creationflags=no_console())
        if not check:
            return result.returncode, result.stdout
        if result.returncode:
            sys.exit(result.returncode)
        return result.stdout
    # stdin given: the output handles are passed explicitly (a windowed launcher child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, env=env, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return None


def optional_script(name, *args):
    """Runs a script whose failure must not stop the game (pictures, icons): its output is shown,
    and a failure is reported."""
    code, output = run_script(name, *args, capture=True, check=False)
    print(output, end='', flush=True)
    if code:
        print(f'{name} failed ({code}); the game starts without it', flush=True)


def launch_prefix(env):
    """Linux integrations chosen in the launcher: MangoHud is its Vulkan layer (an environment
    variable), GameMode a command that starts the game."""
    if WINDOWS:
        return []
    if env.get('GOW3_MANGOHUD') == '1':
        env.setdefault('MANGOHUD', '1')
    if env.get('GOW3_GAMEMODE') == '1':
        if gamemode := shutil.which('gamemoderun'):
            return [gamemode]
        print('GameMode: gamemoderun not found, starting without it', flush=True)
    return []


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def pause_menu_args(game, out):
    """Probe arguments for the copy of R_UI.WAD with the pause menu's Exit to Desktop row
    (scripts/pause_menu.py). The probe mounts it once its hook is in (src/menu_hook.h). The game
    folder is never written."""
    import pause_menu
    try:
        made = pause_menu.build(game, out)
    except OSError as error:
        print(f'Pause menu: {error}; no Exit to Desktop row', flush=True)
        return []
    return [part for path in made.values() for part in ('--pause-menu', str(path))]


def archive_args(game, unlinked, view):
    """--archive NAME FILE for each .psarc the game reads, for scripts/menu_patches.py and
    scripts/xbox_controls.py: the game folder's, and those a drive without links kept out of the
    update view (unlinked: view path -> file), which the game reads in place as /app0/NAME."""
    found = {p.name: p for p in Path(game).iterdir() if p.suffix.lower() == '.psarc' and p.is_file()}
    found.update({Path(link).name: Path(target) for link, target in unlinked.items()
                  if Path(link).parent == Path(view) and Path(link).suffix.lower() == '.psarc'})
    return [part for name, path in sorted(found.items()) for part in ('--archive', name, str(path))]


def save_folder(env, data):
    """The game's saves (as src/runtime_savedata.c): GOW3_SAVEDATA_DIR, else <user>/savedata."""
    return Path(env.get('GOW3_SAVEDATA_DIR') or Path(env.get('GOW3_USER_DIR', data / 'user')) / 'savedata')


def connected_pad_style(probe):
    """The style of player 1's connected gamepad (`gow3-probe --gamepad`), None without one or
    when the probe cannot tell."""
    import pad_style_save
    if not probe:
        return None
    try:
        out = subprocess.run([str(probe), '--gamepad'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True, timeout=15, creationflags=no_console()).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    return pad_style_save.pad_style(out)


def launch_style(env, savedata, title, probe=None):
    """WipEout's prompts at start: player 1's Controller type row as the save holds it
    (scripts/pad_style_save.py), AUTO as the connected gamepad's, else the launcher's controller
    type, else PlayStation. Never auto: the file patches load this style."""
    import pad_style_save
    saved = pad_style_save.saved_style(savedata, title, 'Pad Style P1')
    if saved == pad_style_save.AUTO and (found := connected_pad_style(probe)):
        print(f'Controller type: AUTO, a gamepad with {found} buttons', flush=True)
        return found
    if saved in pad_style_save.STYLES:
        return saved
    style = env.get('GOW3_PAD_STYLE')
    return style if style in pad_style_save.STYLES else 'playstation'


def row_scripts(style, ready):
    """[(script, options)] that draw WipEout's Controls screens' controller picture and put the
    Controller type rows on them; the Xbox picture's layout carries the rows (one patch per entry).
    ready: whether the saves hold the rows' settings (scripts/pad_style_save.py). The game saves a
    row's value without checking that its setting exists, so without them there are no rows."""
    if style == 'xbox':
        return [('xbox_controls.py', [] if ready else ['--no-row'])]
    return [('controller_row.py', [])] if ready else []


def backup_saves(user_dir, env):
    """Backs up the saves (scripts/save_backup.py); False when the backup failed."""
    from save_backup import backup
    try:
        if made := backup(user_dir, savedata=env.get('GOW3_SAVEDATA_DIR')):
            print(f'Saves backed up to {made}', flush=True)
        return True
    except OSError as error:
        print(f'Save backup failed, starting anyway: {error}', flush=True)
        return False


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    # As run.sh: relative paths (GOW3_GAME_DIR, GOW3_DATA_DIR, fsr4_shaders/) are from the port.
    os.chdir(PORT)
    env = os.environ
    data = Path(env.get('GOW3_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    # ponytail: the cache of the removed controller glyphs (God of War III only); drop after 0.2.50.
    shutil.rmtree(out / 'glyphs', ignore_errors=True)
    env.setdefault('GOW3_CONFIG', str(data / 'gow3.ini'))
    # Values this script derived on the previous start ("Apply and restart" inherits them) are
    # derived again from the new options.
    for key in env.pop('GOW3_DERIVED', '').split(','):
        env.pop(key, None)
    derived = []
    sys.path.insert(0, str(PORT / 'scripts'))
    import graphics_options
    try:
        import discord_presence
    except Exception:
        discord_presence = None
    import mods
    import patches
    # The enabled mods' patch files: their "Resolution Patch - <size>" entries are render resolutions too
    # (the launcher and the in-game menu list them; GOW3_EXTRA_RESOLUTIONS tells the game).
    mod_patch_files = mods.enabled_patch_files(env.get('GOW3_MODS_DIR', data / 'mods'),
                                               env.get('GOW3_MODS_CONFIG', data / 'mods.json'),
                                               env.get('GOW3_MODS_ENABLED', '1') != '0',
                                               env.get('GOW3_GAME_DIR', PORT.parent / 'CUSA01623'))
    # A game's own sizes beyond the shared list (WipEout's 2880x1800, #110) come from its profile.
    import game_profiles
    own = game_profiles.PROFILES.get(patches.game_title_id(env.get('GOW3_GAME_DIR', PORT.parent / 'CUSA01623')), {})
    own = own.get('env', {}).get('GOW3_ALLOWED_RESOLUTIONS', '').split(';')
    extra_resolutions = list(dict.fromkeys(s for s in [*patches.resolution_names(mod_patch_files), *own]
                                           if s and s not in graphics_options.RESOLUTIONS))
    env['GOW3_EXTRA_RESOLUTIONS'] = ';'.join(extra_resolutions)
    derived.append('GOW3_EXTRA_RESOLUTIONS')
    graphics, reverted = graphics_options.prepare(env['GOW3_CONFIG'], out / 'graphics_state.json', extra_resolutions)
    if reverted:
        print('Graphics: the last restart never reached the game; previous options restored', flush=True)
    env.update(graphics_options.environment(graphics))
    if not env.get('GOW3_FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['GOW3_FSR411_DIR'] = str(data / 'fsr4_411')

    probe = Path(env['GOW3_PROBE']) if env.get('GOW3_PROBE') else find_executable('gow3-probe' + EXE_SUFFIX)
    if not probe or not probe.is_file():
        fail(f'gow3-probe{EXE_SUFFIX} not found: build it with build.sh or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if WINDOWS and not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    # Instruction sets this CPU lacks (MOVBE, BMI1, LZCNT): the ISA rewrite covers them; exit 20
    # is a set nothing covers.
    features = subprocess.run([str(probe), '--cpu-features'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              text=True, creationflags=no_console())
    if features.returncode == 20:
        print(features.stdout, end='', flush=True)
        sys.exit(20)
    missing = parse_cpu_features(features.stdout)

    game = Path(env.get('GOW3_GAME_DIR', PORT.parent / 'CUSA01623'))
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set GOW3_GAME_DIR).')
    update_view = None
    unlinked = {}  # large files a drive without links kept out of the view: the game reads them in place
    if env.get('GOW3_PATCH_DIR'):
        import game_update
        try:
            update_view = game_update.create_view(game, env['GOW3_PATCH_DIR'], out, unlinked)
        except (ValueError, OSError) as error:
            fail(str(error))
        game = update_view
        print(f'Game and update combined from {env["GOW3_PATCH_DIR"]}', flush=True)
        if unlinked:
            print(f'Game and update: this drive cannot link files; {len(unlinked)} large files are read '
                  'from their folders', flush=True)
    original_game = game.resolve()
    game = Path(run_script('mods.py', game, '--out', out,
                           '--mods-dir', env.get('GOW3_MODS_DIR', data / 'mods'),
                           '--config', env.get('GOW3_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('GOW3_MODS_ENABLED', '1'), capture=True).strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        run_script('prepare.py', game, '--out', out)
        run_script('link_libc.py', game, '--out', out)
        import game_profiles
        profile = game_profiles.profile(game)
        if profile['name']:
            print(f'Game profile: {profile["name"]} ({", ".join(profile["modules"])})', flush=True)
        for name, value in profile.get('env', {}).items():
            env.setdefault(name, value)
        run_script('link_modules.py', game, '--out', out, '--modules', *profile['modules'],
                   '--reserve', str(profile.get('reserve', 0)))
        if WINDOWS:  # function starts for the loader's red-zone protection (src/redzone.cpp)
            run_script('function_starts.py', game, '--out', out, '--modules', *profile['modules'])
        # A failed rewrite prints its reason and the game starts without the table.
        isa_args = []
        if isa_rewrite_wanted(missing, env):
            code, output = run_script('isa_rewrite.py', out / 'boot-linked.bin', '--out', out,
                                      capture=True, check=False)
            print(output, end='', flush=True)
            if code == 0:
                isa_args = ['--isa-rewrite', str(out / 'isa-rewrite.bin')]
        run_script('content_profile.py', game, '--out', out, '--sku', env.get('GOW3_CONTENT_SKU', 'full'))
        psarcs = archive_args(game, unlinked, update_view)
        run_script('menu_patches.py', game, '--out', out, *psarcs,  # Eliminator target slider limit (#89)
                   '--limit', env.get('GOW3_ELIMINATOR_LIMIT', '2000'))
        user_dir = Path(env.get('GOW3_USER_DIR', data / 'user')).resolve()  # saves, GPU caches (below)
        # WipEout: its Controls screens get a Controller type row per player (scripts/controller_row.py),
        # which the game saves; the prompts start in player 1's choice and follow the rows while the
        # game runs (src/runtime_glyphs.c). The Controls screen's controller picture is chosen here.
        wipeout = bool(profile['name'] == 'WipEout Omega Collection' and psarcs)
        if wipeout:
            title = patches.game_title_id(game)
            savedata = save_folder(env, data)
            # The saves first: backed up, then given the rows' settings; the rows only once every
            # save holds them.
            import pad_style_save
            try:
                if backup_saves(user_dir, env):
                    for path in pad_style_save.add_settings(savedata, title, env.get('GOW3_PAD_STYLE')):
                        print(f'Controller type: settings added to {path}', flush=True)
                ready = pad_style_save.settings_ready(savedata, title)
            except OSError as error:
                print(f'Controller type: the save was left as it is ({error})', flush=True)
                ready = False
            if not ready:
                print("Controller type: the save could not take the row's settings; the Controls screens "
                      'stay as they are', flush=True)
            style = launch_style(env, savedata, title, probe)
            if style in ('xbox', 'nintendo'):
                optional_script('wipeout_glyphs.py', game, '--out', out, '--style', style,
                                '--pad-map', env.get('GOW3_PAD_MAP', ''), *psarcs)
            for script, options in row_scripts(style, ready):
                optional_script(script, game, '--out', out, *options, *psarcs)
            optional_script('wipeout_glyphs.py', game, '--out', out, '--swap-config', out / 'glyph_swap.txt',
                            '--pad-map', env.get('GOW3_PAD_MAP', ''), *psarcs)
            env['GOW3_GLYPH_SWAP'] = str(out / 'glyph_swap.txt')
            env['GOW3_GLYPH_STYLE'] = style  # what the file patches above load (src/runtime_glyphs.c)
            derived += ['GOW3_GLYPH_SWAP', 'GOW3_GLYPH_STYLE']
        env['GOW3_FILE_PATCHES'] = str(out / 'file_patches.txt')

        from patches import game_profile_for, patch_requirements, patch_status, selected_patches
        profile = game_profile_for(game)
        engine_fps = 60
        # The window shows the game's icon (BMP: the format SDL loads without extra libraries); on
        # Linux the launcher's own icon (the AppImage ships the one chosen when it was built) and
        # an application id the desktop entry matches, so the shell shows the same icon.
        icon_file = game / 'sce_sys' / 'icon0.png'
        if not WINDOWS:
            env.setdefault('SDL_APP_ID', 'gow3')
            if (PORT / 'launcher' / 'gow3.png').is_file():
                icon_file = PORT / 'launcher' / 'gow3.png'
        try:
            from PIL import Image
            with Image.open(icon_file) as icon:
                icon.convert('RGBA').resize((64, 64), Image.LANCZOS).save(out / 'window_icon.bmp')
            env.setdefault('GOW3_WINDOW_ICON', str(out / 'window_icon.bmp'))
        except (ImportError, OSError):
            pass
        # The temporal upscalers and motion vectors are not calibrated for this game yet: they stay
        # off unless the launcher's experimental switch is on (then gow3.ini decides); linear
        # image readbacks fix its corrupted textures.
        if env.get('GOW3_EXPERIMENTAL_UPSCALER') != '1':
            env.setdefault('GOW3_UPSCALER', 'none')
        else:
            # FSR 3.1 / DLAA smooth the edges instead of the game's multisampling; with both on, a
            # race showed another image in the upper-left quarter (NVIDIA, where it is on by default).
            env['GOW3_NO_MSAA'] = '1'
        env.setdefault('GOW3_READBACK_LINEAR', '1')
        patch_xml = profile[1] if profile else None
        if profile and mod_patch_files:
            # The built-in patches plus the enabled mods' (the same file for the selection and the notes).
            patch_xml = patches.merge_patch_files(profile[1], mod_patch_files, out / 'patches-merged.xml')
        if profile:
            names = selected_patches(patch_xml, profile[2], env.get('GOW3_PATCHES', ''), env.get('GOW3_PATCHES_ONLY') == '1')
            # A mod's patches that are on by default (isEnabled) stay on when the launcher chose the list.
            names += [n for n in patches.default_enabled(mod_patch_files, profile[2]) if n not in names]
            # gow3.ini decides the resolution and frame rate patches (in-game menu, launcher).
            names = graphics_options.patch_names(names, graphics)
            names = graphics_options.restrict_to_available(names, patches.available_patches(patch_xml, profile[2]))
            env['GOW3_PATCHES'] = ';'.join(names)
            env['GOW3_PATCHES_ONLY'] = '1'
        (out / 'patch_status.json').unlink(missing_ok=True)
        run_script('patches.py', '--out', out, '--extra', env.get('GOW3_PATCHES', ''), '--game-dir', game,
                   '--patches-dir', env.get('GOW3_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('GOW3_PATCHES_CONFIG', data / 'patches.json'),
                   *[arg for f in mod_patch_files for arg in ('--mod-patches', f)])
        if profile:
            # Only the patches found in this eboot.bin count: a frame rate patch that was skipped
            # leaves the engine at 60 FPS, and the notes' memory and VBlank needs follow the same list.
            applied = (patch_status(out) or {}).get('applied', [])
            engine_fps = graphics_options.applied_engine_fps(applied)
            dmem, vblank = patch_requirements(patch_xml, applied, profile[2])
            for key, value in (('GOW3_DMEM_MB', dmem), ('GOW3_VBLANK_HZ', vblank)):
                if value and key not in env:
                    env[key] = str(value)
                    derived.append(key)
        if 'GOW3_VBLANK_HZ' not in env:
            env['GOW3_VBLANK_HZ'] = '60'
            derived.append('GOW3_VBLANK_HZ')
        # Without the frame rate patch the engine steps 1/60 s per frame: the present thread must
        # not flip faster (gow3_graphics.h PresentLimit), or the game speeds up. A game whose
        # engine steps by the real time (its profile sets the rate) keeps the profile's.
        if 'GOW3_ENGINE_FPS' not in env:
            env['GOW3_ENGINE_FPS'] = str(engine_fps)
            derived.append('GOW3_ENGINE_FPS')
        engine_fps = int(env['GOW3_ENGINE_FPS'])
        print(f'Engine frame rate: {engine_fps} FPS'
              + ('' if engine_fps > 60 or graphics['engine_fps'] == '60' else
                 ' (the frame rate patch was not found in this eboot.bin)'), flush=True)
        mount_args = pause_menu_args(game, out)
        for link, target in unlinked.items():
            mount_args += ['--mount', f'/app0/{Path(link).relative_to(update_view).as_posix()}={target}']
        env['GOW3_DERIVED'] = ','.join(derived)
        # The guest memory pool is committed in full at start: a resolution patch above 1080p
        # can need more than 16 GB of RAM, and the game then closes when the page file cannot grow.
        import guest_memory
        if warning := guest_memory.memory_warning(env.get('GOW3_DMEM_MB'), guest_memory.physical_memory_mb(),
                                                  env.get('GOW3_STREAM_MEMORY', 'cached'),
                                                  neo=env.get('GOW3_NEO', '').startswith('1')):
            print(warning, flush=True)

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir.mkdir(parents=True, exist_ok=True)
        (user_dir / 'music').mkdir(exist_ok=True)  # custom soundtrack (src/runtime_music.c)
        env.setdefault('GOW3_GPU_USER_DIR', str(user_dir))
        if not wipeout:  # WipEout's were backed up before its settings went in (above)
            backup_saves(user_dir, env)
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        # Windows: the probe starts the new launch and ends, so it waits for this process. Linux:
        # the probe replaces itself with the new launch while this process still waits for it.
        after = ['--after', str(os.getpid())] if WINDOWS else []
        restart = [sys.executable, *this, *after, *args]
        env['GOW3_RESTART_COMMAND'] = subprocess.list2cmdline(restart) if WINDOWS else shlex.join(restart)
        command = [*launch_prefix(env), str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', env.get('GOW3_TIMEOUT', '0'), *isa_args, *mount_args, *args]
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, creationflags=no_console())
        presence = None
        try:
            try:
                presence = discord_presence.Presence(pid=process.pid) if discord_presence else None
            except (OSError, RuntimeError):
                print('Discord presence unavailable; the game will continue', flush=True)
            status = process.wait()
            print(f'Game process exited: {status} (0x{status & 0xffffffff:08X})', flush=True)
            return status
        finally:
            if presence:
                presence.close()
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)
        if update_view:
            game_update.remove_view(update_view)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
