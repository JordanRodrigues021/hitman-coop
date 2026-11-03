#!/usr/bin/env bash
#
# HITMAN Co-op installer for Linux. Installs the ZHMModSDK mod loader (if needed)
# and the HitmanCoop mod into a HITMAN World of Assassination install running
# through Proton/Wine, and can undo it again.
#
#   ./installer.sh                  find the game and install
#   ./installer.sh --game "/path/to/HITMAN 3"
#   ./installer.sh --uninstall
#   ./installer.sh --list-games     print the detected installs, one per line
#
# The game must be the Steam or Epic (including Heroic/Legendary) version.
# The Microsoft Store / Game Pass version is not supported on Windows either.
#
# Ported from installer.ps1 by Jordan Rodrigues.

set -Eeuo pipefail
shopt -s nullglob

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
MOD_NAME="HitmanCoop"
# The build for the game version found (select_payload): files/ for the current game (3.290.x),
# files-3.280/ for players who can't update past 3.280.
PAYLOAD="$SCRIPT_DIR/files"
MANIFEST_NAME="HitmanCoop-install.json"
DEFAULT_UDP_PORT="47047"
LOG_FILE=""
TMP_DIR=""

# When started with sudo, look for the games in the real user's home, not root's.
REAL_HOME=${HOME:-}
if [[ ${EUID:-$(id -u)} -eq 0 && -n ${SUDO_USER:-} ]] && command -v getent >/dev/null 2>&1; then
    _home=$(getent passwd "$SUDO_USER" | cut -d: -f6 || true)
    [[ -n $_home ]] && REAL_HOME=$_home
fi
unset _home 2>/dev/null || true

Say() {
    printf '%s\n' "$*"
    if [[ -n $LOG_FILE ]]; then
        printf '%s\n' "$*" >> "$LOG_FILE" 2>/dev/null || true
    fi
}

Die() {
    local message="ERROR: $*"
    # stderr, so an error inside a $(...) capture is still shown to the user.
    printf '%s\n' "$message" >&2
    if [[ -n $LOG_FILE ]]; then
        printf '%s\n' "$message" >> "$LOG_FILE" 2>/dev/null || true
    fi
    exit 1
}

Usage() {
    cat <<'EOF'
HITMAN Co-op installer for Linux

Usage:
  ./installer.sh                     find the game and install
  ./installer.sh --game PATH         install into a specific game folder
  ./installer.sh --uninstall         undo the installation
  ./installer.sh --list-games        print detected game folders, one per line
  ./installer.sh --quiet             don't ask about the firewall
  ./installer.sh --firewall          add a firewall rule without asking
  ./installer.sh --no-firewall       never touch the firewall

PATH is the folder that contains "Retail" (picking Retail itself also works).

The game must run through Proton/Wine. With Steam, set the launch option
  WINEDLLOVERRIDES="dinput8=n,b" %command%
if the CO-OP window doesn't appear. Heroic and Lutris have an equivalent
environment-variable field in their per-game settings.
EOF
}

cleanup() {
    if [[ -n $TMP_DIR && -d $TMP_DIR ]]; then
        rm -rf -- "$TMP_DIR" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

is_game_dir() {
    local dir=${1:-} retail exe
    [[ -n $dir ]] || return 1
    retail=$(resolve_dest "$dir/Retail")
    exe=$(resolve_dest "$retail/HITMAN3.exe")
    [[ -f $exe ]]
}

is_store_build() {
    [[ -n ${1:-} && -f "$1/Content/Retail/HITMAN3.exe" ]]
}

normalize_game_path() {
    local path=${1:-}
    [[ -n $path ]] || return 0

    # Expand a leading ~ (the shell does not do it inside quoted arguments).
    if [[ $path == '~' ]]; then
        path=$REAL_HOME
    elif [[ $path == '~/'* ]]; then
        path="$REAL_HOME/${path#\~/}"
    fi

    # Strip trailing slashes, but keep "/".
    while [[ $path == */ && $path != / ]]; do
        path=${path%/}
    done

    # Accept the Retail folder itself too.
    if [[ $(basename -- "$path") =~ ^[Rr][Ee][Tt][Aa][Ii][Ll]$ ]]; then
        path=$(dirname -- "$path")
    fi

    printf '%s\n' "$path"
}

# If the exact path exists, return it. Otherwise look for the same name with a
# different case in that directory. This matters when a mod was extracted on
# Windows (HitmanCoop.dll) and the game is on a case-sensitive Linux filesystem
# (hitmancoop.dll).
resolve_dest() {
    local path=$1 dir base match
    if [[ -e $path || -L $path ]]; then
        printf '%s\n' "$path"
        return 0
    fi

    dir=$(dirname -- "$path")
    base=$(basename -- "$path")
    if [[ -d $dir ]]; then
        match=$(find "$dir" -mindepth 1 -maxdepth 1 -iname "$base" -print -quit 2>/dev/null || true)
        if [[ -n $match ]]; then
            printf '%s\n' "$match"
            return 0
        fi
    fi

    printf '%s\n' "$path"
}

remove_case_insensitive() {
    local path
    path=$(resolve_dest "$1")
    if [[ -e $path || -L $path ]]; then
        rm -f -- "$path"
    fi
}

# cp -p fails for a normal user when it cannot preserve another user's
# ownership (for example a game folder installed by root). The content is what
# matters here, so fall back to a plain copy.
copy_file() {
    cp -p -- "$1" "$2" 2>/dev/null || cp -f -- "$1" "$2"
}

# Fix ownership of files this script created or changed when it was run with
# sudo. Without this, Steam/Heroic would later fail to write to them.
fix_owner() {
    if [[ -n ${SUDO_UID:-} && -n ${SUDO_GID:-} ]]; then
        chown -h "$SUDO_UID:$SUDO_GID" -- "$@" 2>/dev/null || true
    fi
}

fix_owner_tree() {
    if [[ -n ${SUDO_UID:-} && -n ${SUDO_GID:-} ]]; then
        chown -R -h "$SUDO_UID:$SUDO_GID" -- "$@" 2>/dev/null || true
    fi
}

# Version resource of a PE file, for the log, the loader warning and choosing the build.
file_version() {
    local file=$1 value=""
    [[ -f $file ]] || return 0

    # VS_FIXEDFILEINFO (signature 0xFEEF04BD): the same numbers Windows shows and the mod reads.
    value=$(python3 - "$file" <<'PY' 2>/dev/null || true
import struct
import sys

with open(sys.argv[1], "rb") as handle:
    data = handle.read()
# The signature also turns up in code: the real one is followed by structure version 1.0.
at = data.find(b"\xbd\x04\xef\xfe")
while at >= 0:
    if at + 16 <= len(data) and struct.unpack_from("<I", data, at + 4)[0] == 0x00010000:
        ms, ls = struct.unpack_from("<II", data, at + 8)
        print(f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}")
        break
    at = data.find(b"\xbd\x04\xef\xfe", at + 1)
PY
    )
    if [[ -n $value ]]; then
        printf '%s\n' "$value"
        return 0
    fi

    if command -v exiftool >/dev/null 2>&1; then
        value=$(exiftool -s3 -ProductVersion -- "$file" 2>/dev/null | head -n 1 || true)
        if [[ -z $value || $value == "-" ]]; then
            value=$(exiftool -s3 -FileVersion -- "$file" 2>/dev/null | head -n 1 || true)
        fi
    fi

    if [[ -z $value || $value == "-" ]] && command -v strings >/dev/null 2>&1; then
        value=$(strings -el "$file" 2>/dev/null | grep -m 1 -E '^[0-9]+(\.[0-9]+){2,3}$' || true)
    fi

    [[ $value == "-" ]] && value=""
    printf '%s\n' "$value"
}

version_gt() {
    local a=$1 b=$2
    [[ -n $a && -n $b && $a != "$b" ]] || return 1
    [[ $(printf '%s\n%s\n' "$a" "$b" | sort -V | tail -n 1) == "$a" ]]
}

game_running() {
    # Wine/Proton gives the game process the exe's name. Don't use pgrep -f
    # here: it would also match a terminal or editor that merely mentions
    # HITMAN3.exe in its command line.
    pgrep -x 'HITMAN3.exe' >/dev/null 2>&1 || pgrep -x 'HITMAN3' >/dev/null 2>&1
}

game_store() {
    local game=$1
    if [[ $game == */steamapps/* ]]; then
        printf 'Steam\n'
    elif [[ -d "$game/.egstore" ]]; then
        printf 'Epic Games\n'
    elif [[ $game == */Heroic/* || $game == */heroic/* || $game == */legendary/* ]]; then
        printf 'Epic Games (Heroic/Legendary)\n'
    else
        printf 'unknown store\n'
    fi
}

# ---------------------------------------------------------------------------
# Finding the game
# ---------------------------------------------------------------------------

FOUND=()
GAMES=()
STEAM_ROOTS=()

add_found() {
    local path=$1
    [[ -n $path ]] || return 0
    is_game_dir "$path" || return 0
    FOUND+=("$(realpath -m -- "$path")")
}

collect_steam_roots() {
    local -a candidates=(
        "${STEAM_ROOT:-}"
        "$REAL_HOME/.steam/steam"
        "$REAL_HOME/.steam/root"
        "$REAL_HOME/.local/share/Steam"
        "${XDG_DATA_HOME:-$REAL_HOME/.local/share}/Steam"
        "$REAL_HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
        "$REAL_HOME/.var/app/com.valvesoftware.Steam/.steam/steam"
        "$REAL_HOME/snap/steam/common/.local/share/Steam"
        "$REAL_HOME/snap/steam/common/.steam/steam"
    )
    local -A seen=()
    local root key

    STEAM_ROOTS=()
    for root in "${candidates[@]}"; do
        [[ -n $root && -d $root ]] || continue
        key=$(realpath -m -- "$root")
        [[ -n ${seen[$key]:-} ]] && continue
        seen[$key]=1
        STEAM_ROOTS+=("$key")
    done
}

find_steam_games() {
    local -a libraries=()
    local -A seen_libs=()
    local root vdf lib key common dir

    collect_steam_roots
    for root in "${STEAM_ROOTS[@]}"; do
        for vdf in "$root/steamapps/libraryfolders.vdf" "$root/config/libraryfolders.vdf"; do
            [[ -f $vdf ]] || continue
            while IFS= read -r lib; do
                [[ -n $lib ]] || continue
                # VDF escapes backslashes; Linux paths normally don't have any.
                lib=${lib//\\\\/\\}
                [[ $lib == /* ]] || lib="$root/$lib"
                [[ -d $lib ]] || continue
                key=$(realpath -m -- "$lib")
                [[ -n ${seen_libs[$key]:-} ]] && continue
                seen_libs[$key]=1
                libraries+=("$key")
            done < <(sed -nE 's/^[[:space:]]*"path"[[:space:]]+"([^"]+)"[[:space:]]*$/\1/Ip' "$vdf")
        done

        key=$(realpath -m -- "$root")
        if [[ -z ${seen_libs[$key]:-} ]]; then
            seen_libs[$key]=1
            libraries+=("$key")
        fi
    done

    for lib in "${libraries[@]}"; do
        common="$lib/steamapps/common"
        [[ -d $common ]] || continue
        add_found "$common/HITMAN 3"
        while IFS= read -r -d '' dir; do
            add_found "$dir"
        done < <(find "$common" -mindepth 1 -maxdepth 1 -type d -iname 'HITMAN*' -print0 2>/dev/null)
    done
}

find_heroic_games() {
    local config_home=${XDG_CONFIG_HOME:-$REAL_HOME/.config}
    local -a files=(
        "$REAL_HOME/.config/legendary/installed.json"
        "$REAL_HOME/.config/heroic/legendaryConfig/legendary/installed.json"
        "$config_home/legendary/installed.json"
        "$config_home/heroic/legendaryConfig/legendary/installed.json"
        "$REAL_HOME/.var/app/com.heroicgameslauncher.hgl/config/heroic/legendaryConfig/legendary/installed.json"
        "$REAL_HOME/.var/app/com.heroicgameslauncher.hgl/config/legendary/installed.json"
        "$REAL_HOME/snap/heroic/current/.config/heroic/legendaryConfig/legendary/installed.json"
    )
    local path

    while IFS= read -r -d '' path; do
        add_found "$path"
    done < <(python3 - "${files[@]}" <<'PY'
import json
import sys

for filename in sys.argv[1:]:
    try:
        with open(filename, encoding="utf-8-sig") as handle:
            data = json.load(handle)
    except Exception:
        continue
    if not isinstance(data, dict):
        continue

    games = data.get("games")
    if isinstance(games, list):
        values = games
    elif isinstance(games, dict):
        values = games.values()
    else:
        values = data.values()
    for game in values:
        if not isinstance(game, dict):
            continue
        title = str(game.get("title") or "")
        path = str(game.get("install_path") or "")
        # The shell checks that the path really contains Retail/HITMAN3.exe.
        if path and title.lower().startswith("hitman"):
            sys.stdout.buffer.write(path.encode("utf-8", "surrogateescape") + b"\0")
PY
    )
}

# Last resort: common "Games" folders (this also catches installs whose
# Heroic/Legendary config was lost).
find_common_games() {
    local base exe
    for base in "$REAL_HOME/Games" "$REAL_HOME/games"; do
        [[ -d $base ]] || continue
        while IFS= read -r -d '' exe; do
            add_found "$(dirname -- "$(dirname -- "$exe")")"
        done < <(find "$base" -maxdepth 6 -type f -iname 'HITMAN3.exe' -ipath '*/Retail/*' -print0 2>/dev/null)
    done
}

find_all_games() {
    local -A seen=()
    local path key

    FOUND=()
    GAMES=()
    find_steam_games
    find_heroic_games
    find_common_games

    for path in "${FOUND[@]}"; do
        key=$(realpath -m -- "$path")
        [[ -n ${seen[$key]:-} ]] && continue
        seen[$key]=1
        GAMES+=("$key")
    done
}

pick_folder() {
    local picked=""

    if [[ -n ${DISPLAY:-}${WAYLAND_DISPLAY:-} ]]; then
        if command -v zenity >/dev/null 2>&1; then
            picked=$(zenity --file-selection --directory \
                --title="Select your HITMAN World of Assassination folder (the one that contains 'Retail')" \
                2>/dev/null || true)
        elif command -v kdialog >/dev/null 2>&1; then
            picked=$(kdialog --getexistingdirectory "$REAL_HOME" \
                --title="Select your HITMAN World of Assassination folder (the one that contains 'Retail')" \
                2>/dev/null || true)
        fi
    fi

    if [[ -z $picked ]]; then
        [[ -t 0 ]] || Die "Couldn't find HITMAN automatically. Pass --game /path/to/HITMAN 3."
        read -r -p "Game folder (contains 'Retail'): " picked || true
        picked=${picked#\"}
        picked=${picked%\"}
        picked=${picked#\'}
        picked=${picked%\'}
    fi

    printf '%s\n' "$picked"
}

RESOLVED_GAME=""

resolve_game() {
    local path picked choice i

    RESOLVED_GAME=""
    if [[ -n $GAME_PATH ]]; then
        path=$(normalize_game_path "$GAME_PATH")
        if is_game_dir "$path"; then
            RESOLVED_GAME=$(realpath -m -- "$path")
            return 0
        fi
        if is_store_build "$path"; then
            Die "This is the Microsoft Store / Game Pass version of HITMAN. Its files are locked by Windows, so no mods can be installed into it. HITMAN Co-op works with the Steam and Epic versions."
        fi
        Die "No Retail/HITMAN3.exe under '$path'."
    fi

    find_all_games

    if ((${#GAMES[@]} == 1)); then
        RESOLVED_GAME=${GAMES[0]}
        return 0
    fi

    if ((${#GAMES[@]} > 1)); then
        Say "Found more than one HITMAN install:"
        for i in "${!GAMES[@]}"; do
            Say "  [$((i + 1))] ${GAMES[$i]}"
        done
        [[ -t 0 ]] || Die "Found more than one HITMAN install. Use --game PATH to choose one."

        read -r -p "Which one? (1-${#GAMES[@]}) " choice || true
        choice=${choice//[[:space:]]/}
        if [[ $choice =~ ^[0-9]+$ ]] && ((10#$choice >= 1 && 10#$choice <= ${#GAMES[@]})); then
            RESOLVED_GAME=${GAMES[$((10#$choice - 1))]}
            return 0
        fi
        Die "No install selected."
    fi

    Say "Couldn't find HITMAN automatically. Please pick the game folder."
    picked=$(pick_folder) || exit 1
    [[ -n $picked ]] || Die "No folder selected."
    picked=$(normalize_game_path "$picked")
    if is_game_dir "$picked"; then
        RESOLVED_GAME=$(realpath -m -- "$picked")
        return 0
    fi
    if is_store_build "$picked"; then
        Die "That is the Microsoft Store / Game Pass version of HITMAN, which can't be modded. HITMAN Co-op works with the Steam and Epic versions."
    fi
    Die "That folder doesn't contain Retail/HITMAN3.exe."
}

# ---------------------------------------------------------------------------
# mods.ini handling
# ---------------------------------------------------------------------------

enable_mod_ini() {
    local ini=$1 eol=$'\n'

    if [[ -f $ini ]] && grep -qiE '^[[:space:]]*\[HitmanCoop\][[:space:]]*$' "$ini"; then
        return 0
    fi

    if [[ -f $ini ]] && grep -q $'\r' "$ini" 2>/dev/null; then
        eol=$'\r\n'
    fi

    # Keep the existing last line intact if it has no line ending.
    if [[ -s $ini ]] && [[ $(tail -c 1 -- "$ini" | wc -l) -eq 0 ]]; then
        printf '%s' "$eol" >> "$ini"
    fi
    printf '[%s]%s' "$MOD_NAME" "$eol" >> "$ini"
    fix_owner "$ini"
}

disable_mod_ini() {
    local ini=$1 tmp
    [[ -f $ini ]] || return 0

    tmp=$(mktemp "$ini.XXXXXX")
    awk -v mod="$MOD_NAME" '
        function trim(s) {
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", s)
            return s
        }
        BEGIN { skip = 0 }
        {
            line = $0
            trimmed = trim(line)
            if (trimmed ~ /^\[.*\]$/) {
                skip = (tolower(trimmed) == "[" tolower(mod) "]")
            }
            if (!skip) {
                print line
            }
        }
    ' "$ini" > "$tmp"

    chmod --reference="$ini" -- "$tmp" 2>/dev/null || true
    mv -f -- "$tmp" "$ini"
    fix_owner "$ini"
}

# ---------------------------------------------------------------------------
# Firewall (the closest Linux equivalent of the Windows program rule)
# ---------------------------------------------------------------------------

read_udp_port() {
    local ini port=""
    ini=$(resolve_dest "$RETAIL/mods/hitmancoop.ini")

    if [[ -f $ini ]]; then
        port=$(awk -F= '
            /^[[:space:]]*\[/ {
                section = tolower($0)
                gsub(/[[:space:]\[\]]/, "", section)
            }
            tolower(section) == "general" && tolower($1) ~ /^[[:space:]]*port[[:space:]]*$/ {
                value = $2
                gsub(/[[:space:]]/, "", value)
                print value
                exit
            }
        ' "$ini")
    fi

    if [[ $port =~ ^[0-9]+$ ]] && ((port >= 1 && port <= 65535)); then
        printf '%s\n' "$port"
    else
        printf '%s\n' "$DEFAULT_UDP_PORT"
    fi
}

firewall_is_active() {
    if command -v systemctl >/dev/null 2>&1; then
        systemctl is-active --quiet ufw 2>/dev/null && return 0
        systemctl is-active --quiet firewalld 2>/dev/null && return 0
    fi
    return 1
}

add_firewall_rule() {
    local port=$1

    if ! firewall_is_active; then
        Say "No active ufw/firewalld service found; no firewall rule was added. If you use nftables/iptables and host by IP, allow UDP $port yourself."
        return 0
    fi

    if systemctl is-active --quiet ufw 2>/dev/null && command -v ufw >/dev/null 2>&1; then
        if [[ $EUID -eq 0 ]]; then
            ufw allow "$port/udp" >/dev/null 2>&1 && {
                Say "Added a firewall rule for UDP $port so friends can connect when you host."
                return 0
            }
        elif command -v sudo >/dev/null 2>&1; then
            sudo ufw allow "$port/udp" >/dev/null 2>&1 && {
                Say "Added a firewall rule for UDP $port so friends can connect when you host."
                return 0
            }
        fi
    fi

    if systemctl is-active --quiet firewalld 2>/dev/null && command -v firewall-cmd >/dev/null 2>&1; then
        if [[ $EUID -eq 0 ]]; then
            firewall-cmd --permanent --add-port="$port/udp" >/dev/null 2>&1 &&
                firewall-cmd --reload >/dev/null 2>&1 && {
                Say "Added a firewall rule for UDP $port so friends can connect when you host."
                return 0
            }
        elif command -v sudo >/dev/null 2>&1; then
            sudo firewall-cmd --permanent --add-port="$port/udp" >/dev/null 2>&1 &&
                sudo firewall-cmd --reload >/dev/null 2>&1 && {
                Say "Added a firewall rule for UDP $port so friends can connect when you host."
                return 0
            }
        fi
    fi

    Say "Couldn't add the firewall rule automatically. If you host by IP, allow UDP $port yourself."
    return 1
}

maybe_add_firewall() {
    local port answer
    port=$(read_udp_port)

    if ((FIREWALL)); then
        add_firewall_rule "$port" || true
    elif ((!NO_FIREWALL && !QUIET)); then
        [[ -t 0 ]] || return 0
        read -r -p "Allow incoming direct connections through the firewall (UDP $port)? Join codes work without this. (y/N) " answer || true
        if [[ $answer =~ ^[Yy] ]]; then
            add_firewall_rule "$port" || true
        else
            Say "Skipped the firewall rule (join codes still work)."
        fi
    fi
}

# ---------------------------------------------------------------------------
# Install / uninstall
# ---------------------------------------------------------------------------

install_mod() {
    local game=$1 retail theirs ours theirs_version ours_version
    local stamp backup backup_rel file rel dest actual_rel
    local -a added=() replaced=()
    local manifest tmp_added tmp_replaced

    is_game_dir "$game" || Die "No Retail/HITMAN3.exe under '$game'."
    retail=$(resolve_dest "$game/Retail")

    if game_running; then
        Die "HITMAN is running. Close the game and run the installer again."
    fi

    [[ -w $retail ]] ||
        Die "The game folder is not writable by $(id -un). Fix its ownership, or run the installer with sudo."

    RETAIL=$retail
    LOG_FILE="$retail/HitmanCoop-install.log"
    if ! : >> "$LOG_FILE" 2>/dev/null; then
        LOG_FILE=""
        Say "Note: couldn't write HitmanCoop-install.log to the game folder."
    else
        fix_owner "$LOG_FILE"
    fi

    Say "---- $(date '+%Y-%m-%d %H:%M:%S') installing HITMAN Co-op"

    local version
    version=$(file_version "$retail/HITMAN3.exe")
    [[ -n $version ]] || version="unknown"
    Say "Game: HITMAN World of Assassination $version - $(game_store "$game") - $game"

    # The build for this game version: each needs its own mod loader (ZHMModSDK) and DLL.
    local build="3.290"
    PAYLOAD="$SCRIPT_DIR/files"
    if [[ $version == 3.280.* && -f "$SCRIPT_DIR/files-3.280/mods/$MOD_NAME.dll" ]]; then
        PAYLOAD="$SCRIPT_DIR/files-3.280"
        build="3.280"
    fi

    [[ -f "$PAYLOAD/mods/$MOD_NAME.dll" ]] ||
        Die "The installer's 'files' folder is missing. Extract the whole archive before running installer.sh."

    Say "Installing the HITMAN Co-op build for game $build.x."
    if [[ $version != "$build"* ]]; then
        Say "Note: there's no HITMAN Co-op build for game $version yet; the $build one is installed. Co-op stays off in the game (it says so) until you update the game; the game itself runs normally."
    fi

    # An existing mod loader: ours replaces it (the mod is built for this one),
    # and it is backed up and put back on uninstall.
    theirs="$retail/ZHMModSDK.dll"
    ours="$PAYLOAD/ZHMModSDK.dll"
    if [[ -f $theirs && -f $ours ]] && ! cmp -s -- "$theirs" "$ours"; then
        theirs_version=$(file_version "$theirs")
        ours_version=$(file_version "$ours")
        Say "Found mod loader (ZHMModSDK) ${theirs_version:-unknown}; HITMAN Co-op needs ${ours_version:-unknown}, so it is installed instead. The old one is backed up and comes back if you uninstall."
        if version_gt "$theirs_version" "$ours_version"; then
            Say "Your other SDK mods were made for $theirs_version and may not load with $ours_version until you uninstall HITMAN Co-op."
        fi
    fi

    stamp=$(date '+%Y%m%d-%H%M%S')
    backup="$retail/HitmanCoop-backup/$stamp"
    if [[ -e $backup ]]; then
        backup="$backup-$$"
    fi
    backup_rel="HitmanCoop-backup/$(basename -- "$backup")"

    while IFS= read -r -d '' file; do
        rel=${file#"$PAYLOAD"/}
        case $rel in
            /*|../*|*/../*|*/..)
                Die "Unsafe file in the installer's 'files' folder: $rel"
                ;;
        esac
        dest=$(resolve_dest "$retail/$rel")
        actual_rel=${dest#"$retail"/}
        if [[ $actual_rel == "$dest" ]]; then
            Die "Refusing to write outside the game folder: $dest"
        fi

        # Already our file from an earlier install: nothing to back up.
        if [[ -f $dest ]] && cmp -s -- "$file" "$dest"; then
            continue
        fi

        if [[ -e $dest || -L $dest ]]; then
            mkdir -p -- "$(dirname -- "$backup/$actual_rel")"
            copy_file "$dest" "$backup/$actual_rel"
            replaced+=("$actual_rel")
        else
            added+=("$actual_rel")
        fi

        mkdir -p -- "$(dirname -- "$dest")"
        cp -f -- "$file" "$dest"
        fix_owner "$dest"
    done < <(find "$PAYLOAD" -type f -print0)

    enable_mod_ini "$retail/mods.ini"

    # Everything really there (Wine, a file manager or an antivirus can quietly
    # block a DLL).
    while IFS= read -r -d '' file; do
        rel=${file#"$PAYLOAD"/}
        dest=$(resolve_dest "$retail/$rel")
        if [[ ! -f $dest ]] || ! cmp -s -- "$file" "$dest"; then
            Die "Couldn't put $(basename -- "$file") into the game folder. If something blocked it, allow it (it is the ZHMModSDK mod loader) and install again."
        fi
    done < <(find "$PAYLOAD" -type f -print0)

    if ! grep -qiE '^[[:space:]]*\[HitmanCoop\][[:space:]]*$' "$retail/mods.ini"; then
        Die "Couldn't turn the mod on in Retail/mods.ini."
    fi

    # Remember what we did, so uninstall can put things back.
    manifest="$retail/$MANIFEST_NAME"
    tmp_added="$TMP_DIR/added"
    tmp_replaced="$TMP_DIR/replaced"
    : > "$tmp_added"
    : > "$tmp_replaced"
    for rel in "${added[@]}"; do
        printf '%s\0' "$rel" >> "$tmp_added"
    done
    for rel in "${replaced[@]}"; do
        printf '%s\0' "$rel" >> "$tmp_replaced"
    done

    python3 - "$manifest" "$stamp" "$backup_rel" "$tmp_added" "$tmp_replaced" <<'PY'
import json
import os
import sys

manifest, stamp, backup_rel, added_file, replaced_file = sys.argv[1:6]


def read_nul(path):
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError:
        return []
    return [item.decode("utf-8", "surrogateescape") for item in data.split(b"\0") if item]


def safe_rel(path):
    if not isinstance(path, str):
        return None
    path = path.replace("\\", "/")
    while path.startswith("./"):
        path = path[2:]
    if not path or path.startswith("/"):
        return None
    if any(part in ("", ".", "..") for part in path.split("/")):
        return None
    return path


previous = {}
if os.path.isfile(manifest):
    try:
        with open(manifest, encoding="utf-8-sig") as handle:
            previous = json.load(handle)
    except Exception as error:
        print(f"Warning: ignoring unreadable manifest {manifest}: {error}", file=sys.stderr)
        previous = {}
if not isinstance(previous, dict):
    previous = {}


def previous_list(key):
    value = previous.get(key)
    if value is None:
        return []
    if isinstance(value, str):
        value = [value]
    if not isinstance(value, list):
        return []
    result = []
    for item in value:
        item = safe_rel(item)
        if item is not None and item not in result:
            result.append(item)
    return result


added = previous_list("added")
for item in read_nul(added_file):
    item = safe_rel(item)
    if item is not None and item not in added:
        added.append(item)

backups = previous_list("backups")
if read_nul(replaced_file):
    folder = safe_rel(backup_rel)
    if folder is not None and folder not in backups:
        backups.append(folder)

data = {
    "installed": stamp,
    "added": added,
    "backups": backups,
}

tmp = manifest + ".tmp"
with open(tmp, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2)
    handle.write("\n")
os.replace(tmp, manifest)
PY

    fix_owner "$manifest"
    if [[ -d "$retail/HitmanCoop-backup" ]]; then
        fix_owner_tree "$retail/HitmanCoop-backup"
    fi
    if [[ -d "$retail/mods" ]]; then
        fix_owner "$retail/mods"
    fi

    maybe_add_firewall

    Say ""
    Say "Installed HITMAN Co-op into $game"
    Say "Start the game, press ~ (or ^ on some keyboards) and click CO-OP to host or join."
    Say "Works with the game online or offline: co-op never uses IOI's servers."
    Say ""
    Say "If the CO-OP window doesn't appear, set this for the game:"
    Say "  Steam:  Properties > Launch Options: WINEDLLOVERRIDES=\"dinput8=n,b\" %command%"
    Say "  Heroic: Settings > Environment Variables: WINEDLLOVERRIDES = dinput8=n,b"
    Say "  Lutris: Configure > System options > Environment variables: WINEDLLOVERRIDES = dinput8=n,b"
}

manifest_list() {
    local manifest=$1 key=$2
    python3 - "$manifest" "$key" <<'PY'
import json
import sys

manifest, key = sys.argv[1], sys.argv[2]


def safe_rel(path):
    if not isinstance(path, str):
        return None
    path = path.replace("\\", "/")
    while path.startswith("./"):
        path = path[2:]
    if not path or path.startswith("/"):
        return None
    if any(part in ("", ".", "..") for part in path.split("/")):
        return None
    return path


try:
    with open(manifest, encoding="utf-8-sig") as handle:
        data = json.load(handle)
except Exception as error:
    print(f"Warning: couldn't read {manifest}: {error}", file=sys.stderr)
    sys.exit(0)

if not isinstance(data, dict):
    sys.exit(0)
value = data.get(key)
if value is None:
    sys.exit(0)
if isinstance(value, str):
    value = [value]
if not isinstance(value, list):
    sys.exit(0)

seen = set()
for item in value:
    item = safe_rel(item)
    if item is not None and item not in seen:
        seen.add(item)
        sys.stdout.buffer.write(item.encode("utf-8", "surrogateescape") + b"\0")
PY
}

uninstall_mod() {
    local game=$1 retail ini manifest rel backup source file dest
    local -a added=() backups=() sorted=() other=()
    local had_loader=0

    is_game_dir "$game" || Die "No Retail/HITMAN3.exe under '$game'."
    retail=$(resolve_dest "$game/Retail")

    if game_running; then
        Die "HITMAN is running. Close the game and run the uninstaller again."
    fi

    [[ -w $retail ]] ||
        Die "The game folder is not writable by $(id -un). Fix its ownership, or run the installer with sudo."

    RETAIL=$retail
    LOG_FILE=""
    ini="$retail/mods.ini"
    manifest="$retail/$MANIFEST_NAME"

    disable_mod_ini "$ini"
    remove_case_insensitive "$retail/mods/$MOD_NAME.dll"

    # Keep the SDK loader if other SDK mods are still enabled.
    if [[ -f $ini ]]; then
        mapfile -t other < <(awk '
            function trim(s) {
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", s)
                return s
            }
            {
                trimmed = trim($0)
                if (trimmed ~ /^\[.+\]$/ && tolower(trimmed) != "[sdk]") {
                    print trimmed
                }
            }
        ' "$ini")
    fi

    if [[ -f $manifest ]]; then
        mapfile -d '' -t added < <(manifest_list "$manifest" added)
        mapfile -d '' -t backups < <(manifest_list "$manifest" backups)

        if ((${#backups[@]} > 0)); then
            had_loader=1
        fi

        # Files we added (the SDK loader, if it wasn't there before) are removed,
        # unless other SDK mods (added since) still need the loader. A loader of
        # their own that we replaced comes back from the backup below, with what
        # it had.
        if ((${#other[@]} > 0 && !had_loader)); then
            Say "Other SDK mods are enabled, so the mod loader stays."
            remove_case_insensitive "$retail/mods/$MOD_NAME.dll"
        else
            for rel in "${added[@]}"; do
                remove_case_insensitive "$retail/$rel"
            done
        fi

        # Files we replaced get their oldest backup back.
        if ((${#backups[@]} > 0)); then
            mapfile -d '' -t sorted < <(printf '%s\0' "${backups[@]}" | sort -z)
            for backup in "${sorted[@]}"; do
                source="$retail/$backup"
                [[ -d $source ]] || continue

                while IFS= read -r -d '' file; do
                    rel=${file#"$source"/}
                    dest=$(resolve_dest "$retail/$rel")
                    mkdir -p -- "$(dirname -- "$dest")"
                    copy_file "$file" "$dest"
                    fix_owner "$dest"
                done < <(find "$source" -type f -print0)
                break
            done
        fi

        rm -f -- "$manifest"
        rm -rf -- "$retail/HitmanCoop-backup" 2>/dev/null || true

        # An empty mods.ini only held our section.
        if [[ -f $ini && ! -s $ini ]]; then
            rm -f -- "$ini"
        fi
    fi

    rm -f -- "$retail/HitmanCoop-install.log" 2>/dev/null || true
    if [[ -d "$retail/mods" ]]; then
        fix_owner "$retail/mods"
        if [[ -z $(find "$retail/mods" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null) ]]; then
            rmdir -- "$retail/mods" 2>/dev/null || true
        fi
    fi

    Say "Removed HITMAN Co-op from $game"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

GAME_PATH=""
UNINSTALL=0
NO_FIREWALL=0
FIREWALL=0
LIST_GAMES=0
QUIET=0

while (($#)); do
    case $1 in
        -g|--game|--game-path)
            (($# >= 2)) || Die "Missing value for $1."
            GAME_PATH=$2
            shift 2
            ;;
        --game=*)
            GAME_PATH=${1#*=}
            shift
            ;;
        -u|--uninstall)
            UNINSTALL=1
            shift
            ;;
        -l|--list-games)
            LIST_GAMES=1
            shift
            ;;
        -q|--quiet)
            QUIET=1
            shift
            ;;
        --firewall)
            FIREWALL=1
            shift
            ;;
        --no-firewall)
            NO_FIREWALL=1
            shift
            ;;
        -h|--help)
            Usage
            exit 0
            ;;
        --)
            shift
            while (($#)); do
                [[ -z $GAME_PATH ]] || Die "More than one game folder given."
                GAME_PATH=$1
                shift
            done
            ;;
        -*)
            Die "Unknown option: $1 (try --help)"
            ;;
        *)
            [[ -z $GAME_PATH ]] || Die "More than one game folder given."
            GAME_PATH=$1
            shift
            ;;
    esac
done

command -v python3 >/dev/null 2>&1 || Die "python3 is required to read and write the install manifest."
for cmd in find sed awk grep sort cp mkdir rm mv date realpath cmp mktemp pgrep chmod chown basename dirname head tail wc id cut; do
    command -v "$cmd" >/dev/null 2>&1 || Die "Required command not found: $cmd"
done

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/hitmancoop.XXXXXX") ||
    Die "Couldn't create a temporary directory."

if ((LIST_GAMES)); then
    find_all_games
    ((${#GAMES[@]} == 0)) || printf '%s\n' "${GAMES[@]}"
    exit 0
fi

resolve_game
GAME=$RESOLVED_GAME

if ((UNINSTALL)); then
    uninstall_mod "$GAME"
else
    install_mod "$GAME"
fi
