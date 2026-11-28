#!/bin/bash
# Smoke test for a packaged Windows build, run by CI in Git Bash on the build machine.
#
# It cannot test streaming (the build machine has no GPU, display driver or client), but it
# does prove that the packaged program starts, serves its web interface, accepts a login,
# and that the settings and actions TunaStream adds are wired up. It also exercises the
# installer script that lets TunaStream sit next to Sunshine or Apollo.
#
# usage: smoke-test.sh <path to the portable .zip> <path to repo root> <expected version>

set -u
zip_path=$(cygpath -u "$1")
repo_root=$(cygpath -u "$2")
expected_version=$3

failures=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
expect() {  # expect <description> <actual> <wanted substring>
  if [[ "$2" == *"$3"* ]]; then pass "$1"; else fail "$1 (wanted '$3', got '${2:0:300}')"; fi
}

work=$(mktemp -d)
echo "Working in $work"

############################################################
# 1. Side-by-side install script
############################################################
echo "== Side-by-side install script =="
root="$work/coexist"
mkdir -p "$root/scripts" "$root/config"
cp "$repo_root/src_assets/windows/misc/migration/coexist-setup.bat" "$root/scripts/"
bat_win=$(cygpath -w "$root/scripts/coexist-setup.bat")
conf="$root/config/tunastream.conf"
run_bat() { cmd.exe //c "$bat_win" > "$work/bat.out" 2>&1; echo "exit=$?" >> "$work/bat.out"; }

# This part creates and deletes a stand-in SunshineService. On a PC with a real Sunshine or Apollo
# that would remove the real service, so refuse to run anywhere one is installed.
for real_service in SunshineService ApolloService; do
  if sc.exe query "$real_service" > /dev/null 2>&1; then
    echo "::error::$real_service is installed on this machine. This test deletes that service, so it only runs on a clean build machine."
    echo "RESULT: 1 failure(s)"
    exit 1
  fi
done

# No other host installed: nothing should be written
run_bat
expect "script exits cleanly with no other host" "$(cat "$work/bat.out")" "exit=0"
if [ -f "$conf" ]; then fail "config left alone with no other host (file was created)"; else pass "config left alone with no other host"; fi

# Pretend Sunshine is installed, with existing settings that do not end in a newline
sc.exe create SunshineService binPath= "C:\\Windows\\System32\\svchost.exe" start= demand > "$work/sc.out" 2>&1 \
  || fail "could not create a stand-in SunshineService: $(cat "$work/sc.out")"
printf 'min_log_level = 2' > "$conf"
run_bat
conf_text=$(tr -d '\r' < "$conf")
expect "script exits cleanly with Sunshine present" "$(cat "$work/bat.out")" "exit=0"
expect "own port is set" "$conf_text" "port = 48989"
expect "host is given a distinct name" "$conf_text" "tunastream_name = ${COMPUTERNAME}-TunaStream"
if grep -q -x "min_log_level = 2" <<< "$conf_text"; then pass "existing setting kept on its own line"; else fail "existing setting kept on its own line (file: $conf_text)"; fi

# Running it again must not add a second port
run_bat
count=$(tr -d '\r' < "$conf" | grep -c "^port")
if [ "$count" = "1" ]; then pass "running twice sets the port once"; else fail "running twice sets the port once (found $count)"; fi

# A port the user already chose must be respected
printf 'port = 50000\r\n' > "$conf"
run_bat
conf_text=$(tr -d '\r' < "$conf")
if [[ "$conf_text" == *"48989"* ]]; then fail "user's own port is respected (file: $conf_text)"; else pass "user's own port is respected"; fi

sc.exe delete SunshineService > /dev/null 2>&1

############################################################
# 2. The packaged program starts and serves its web interface
############################################################
echo "== Packaged program =="
app="$work/app"
mkdir -p "$app"
unzip -q "$zip_path" -d "$app" || fail "could not unzip $zip_path"
exe=$(find "$app" -iname "tunastream.exe" | head -n 1)
if [ -z "$exe" ]; then
  fail "tunastream.exe found in the portable zip"
  echo "RESULT: $failures failure(s)"
  exit 1
fi
pass "tunastream.exe found in the portable zip"
app_dir=$(dirname "$exe")

# Without these the virtual display driver can't be installed, and every stream falls back to the PC's screen
for driver_file in SudoVDA.dll SudoVDA.inf sudovda.cat nefconc.exe install.bat; do
  if [ -n "$(find "$app_dir/drivers/sudovda" -maxdepth 1 -iname "$driver_file" 2>/dev/null)" ]; then
    pass "virtual display driver package has $driver_file"
  else
    fail "virtual display driver package has $driver_file"
  fi
done

(cd "$app_dir" && ./tunastream.exe > "$work/run.log" 2>&1 &)

base="https://localhost:47990"
up=""
for _ in $(seq 1 90); do
  code=$(curl -sk -o /dev/null -w "%{http_code}" --max-time 3 "$base/api/configLocale" 2>/dev/null)
  if [ "$code" = "200" ]; then up=1; break; fi
  sleep 2
done
if [ -z "$up" ]; then
  fail "web interface answers within 3 minutes"
  echo "--- program output ---"; tail -n 40 "$work/run.log"
  taskkill //F //IM tunastream.exe > /dev/null 2>&1
  echo "RESULT: $failures failure(s)"
  exit 1
fi
pass "web interface answers"

json() { python -c "import json,sys; d=json.load(sys.stdin); print(d.get('$1'))" 2>/dev/null; }
jar="$work/cookies.txt"
post() { curl -sk --max-time 20 -b "$jar" -c "$jar" -H "Content-Type: application/json" -X POST -d "$2" "$base$1"; }
get() { curl -sk --max-time 20 -b "$jar" -c "$jar" "$base$1"; }

# First run sends you to account creation
redirect=$(curl -sk -o /dev/null -w "%{http_code} %{redirect_url}" --max-time 20 "$base/")
expect "first visit is sent to account creation" "$redirect" "/welcome"
expect "account creation page is TunaStream's" "$(get /welcome)" "TunaStream"

expect "account can be created" "$(post /api/password '{"newUsername":"ci","newPassword":"ci-Pass-123","confirmNewPassword":"ci-Pass-123"}' | json status)" "True"
# Nothing is served without logging in, and a wrong password is refused
expect "config is refused before login" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 "$base/api/config")" "401"
expect "wrong password is refused" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 -H "Content-Type: application/json" -X POST -d '{"username":"ci","password":"wrong"}' "$base/api/login")" "401"
# A successful login answers 200 with a session cookie and no body
expect "login works" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 -c "$jar" -H "Content-Type: application/json" -X POST -d '{"username":"ci","password":"ci-Pass-123"}' "$base/api/login")" "200"
if grep -q "auth" "$jar" 2>/dev/null; then pass "login sets a session cookie"; else fail "login sets a session cookie"; fi

config=$(get /api/config)
expect "config reports Windows" "$(json platform <<< "$config")" "windows"
expect "config reports this build's version" "$(json version <<< "$config")" "$expected_version"
if [[ "$config" == *"vdisplayStatus"* ]]; then pass "virtual display status is reported"; else fail "virtual display status is reported"; fi

# Pages
for page in "" pin apps config troubleshooting password setup help live remote pad; do
  body=$(get "/$page")
  if [[ "$body" == *"<title>TunaStream</title>"* && "$body" == *'id="app"'* ]]; then pass "page /$page loads"; else fail "page /$page loads (${#body} bytes)"; fi
done

# Branding files the pages and the tray rely on
for file in images/tunastream.ico images/logo-tunastream-45.png assets/css/tunastream.css; do
  expect "file /$file is served" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 -b "$jar" "$base/$file")" "200"
done

# TunaStream additions
expect "rumble test answers, with nobody streaming" "$(post /api/rumble-test '{}' | json clients)" "0"
expect "app list is served" "$(get /api/apps)" "Desktop"
status=$(get /api/status)
expect "status answers" "$(json status <<< "$status")" "True"
expect "status lists no stream health with nobody streaming" "$(python -c "import json,sys; print(len(json.load(sys.stdin)['streams']))" <<< "$status" 2>/dev/null)" "0"
expect "status lists no controllers with nobody streaming" "$(python -c "import json,sys; print(len(json.load(sys.stdin)['controllers']))" <<< "$status" 2>/dev/null)" "0"

expect "setup is not yet done on a fresh install" "$(get /api/config | json setup_complete)" "None"
expect "settings can be saved" "$(post /api/config '{"end_session_on_disconnect":"enabled","virtual_display_primary":"disabled","headless_mode":"disabled"}' | json status)" "True"
config=$(get /api/config)
expect "saved setting is read back: end session on disconnect" "$(json end_session_on_disconnect <<< "$config")" "enabled"
expect "saved setting is read back: virtual display primary" "$(json virtual_display_primary <<< "$config")" "disabled"
expect "setup can be marked done" "$(post /api/config '{"setup_complete":"enabled"}' | json status)" "True"
expect "setup done is read back" "$(get /api/config | json setup_complete)" "enabled"

expect "status reports virtual screen repair state" "$(get /api/status | json vdisplayRepairing)" "False"
expect "play-here refuses with nothing paused" "$(post /api/apps/continue-here '{}' | json status)" "False"
expect "installed games and programs can be listed" "$(get /api/apps/discover | json status)" "True"
expect "status lists other streaming hosts" "$(get /api/status | json otherHosts)" "["
expect "other-host control refuses unknown services" "$(post /api/other-hosts '{"service":"Spooler","run":false}' | json status)" "False"
expect "end session answers with nothing running" "$(post /api/apps/close '{}' | json status)" "True"
expect "files waiting for devices can be listed" "$(get /api/files | json status)" "True"
expect "a file can't be sent to an unpaired device" "$(curl -sk --max-time 20 -b "$jar" -c "$jar" -H "Content-Type: application/octet-stream" -X POST --data-binary "hello" "$base/api/files/send?uuid=not-a-device&name=a.txt" | json status)" "False"
expect "picked apps can be added" "$(post /api/apps/add-many '{"apps":[{"name":"Smoke Test App","cmd":"cmd.exe /c exit","detached":"","workingDir":""}]}' | json added)" "1"
expect "added app is in the app list" "$(get /api/apps)" "Smoke Test App"

# Phone remote and phone controllers
expect "phone remote refuses an unknown action" "$(post /api/remote '{"action":"dance"}' | json status)" "False"
expect "phone remote presses a key" "$(post /api/remote '{"action":"key","key":"escape"}' | json status)" "True"
expect "phone remote refuses an unknown key" "$(post /api/remote '{"action":"key","key":"nope"}' | json status)" "False"
pad=$(post /api/pad '{"pad":"smoke-test-pad-1","name":"CI","buttons":4096}')
# No ViGEmBus on the build machine: the controller can't be added, and it says why
if [[ "$pad" == *'"status":true'* || "$pad" == *"ViGEmBus"* ]]; then pass "phone controller answers"; else fail "phone controller answers ($pad)"; fi
expect "phone controller leaves" "$(post /api/pad/leave '{"pad":"smoke-test-pad-1"}' | json status)" "True"
player=$(post /api/pad/invite '{}' | json token)
if [[ "$player" == tsp_* ]]; then pass "a player link can be made"; else fail "a player link can be made ($player)"; fi
expect "a player link opens the controller page" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 "$base/pad?token=$player")" "200"
expect "a player link can't read the settings" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 -H "Authorization: Bearer $player" "$base/api/config")" "401"
expect "a player link can't drive the mouse" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 -H "Authorization: Bearer $player" -H "Content-Type: application/json" -X POST -d '{"action":"click","button":1}' "$base/api/remote")" "401"

# TunaHome linking: a TV asks, the owner allows, the TV collects an inbox-only token
anon() { curl -sk --max-time 20 "$@"; }
req=$(anon -H "Content-Type: application/json" -X POST -d '{"name":"CI TV"}' "$base/api/tunahome/request" | json request)
if [ -n "$req" ] && [ "$req" != "None" ]; then pass "a TV can ask to be linked"; else fail "a TV can ask to be linked ($req)"; fi
expect "the TV waits until allowed" "$(anon "$base/api/tunahome/claim?request=$req" | json waiting)" "True"
expect "the home page lists the TV asking" "$(get /api/tunahome/requests)" "CI TV"
expect "the owner can allow the TV" "$(post /api/tunahome/answer "{\"request\":\"$req\",\"allow\":true}" | json status)" "True"
home=$(anon "$base/api/tunahome/claim?request=$req" | json token)
if [[ "$home" == tsh_* ]]; then pass "the TV collects its token"; else fail "the TV collects its token ($home)"; fi
expect "the token is handed over once" "$(anon "$base/api/tunahome/claim?request=$req" | json status)" "False"
expect "the linked TV reads its inbox" "$(anon -H "Authorization: Bearer $home" "$base/api/tv-inbox?after=0" | json status)" "True"
expect "a message reaches a linked TV with nobody streaming" "$(post /api/tv-message '{"text":"Dinner is ready"}' | json status)" "True"
expect "the message is in the TV's inbox" "$(anon -H "Authorization: Bearer $home" "$base/api/tv-inbox?after=0")" "Dinner is ready"
post /api/apps/add-many '{"apps":[{"name":"CI Site","detached":"cmd /c start \"\" \"https://www.example.com/tv\""}]}' > /dev/null
expect "the TV lists the websites for its corner" "$(anon -H "Authorization: Bearer $home" "$base/api/tunahome/sites")" "https://www.example.com/tv"
expect "the TV can't open what isn't a website" "$(anon -o /dev/null -w "%{http_code}" -H "Authorization: Bearer $home" -H "Content-Type: application/json" -X POST -d '{"name":"Nope"}' "$base/api/tunahome/corner")" "400"
expect "the TV may show the Watch picture" "$(anon -o /dev/null -w "%{http_code}" "$base/watch?token=$home&embed=1")" "200"
expect "the TV's token can't read the settings" "$(anon -o /dev/null -w "%{http_code}" -H "Authorization: Bearer $home" "$base/api/config")" "401"
expect "quick messages can be set" "$(post /api/quick-messages '{"messages":["Game night!"," ","Bath time"]}' | json messages)" "['Game night!', 'Bath time']"
expect "quick messages are read back" "$(get /api/quick-messages)" "Bath time"
expect "buzz answers with nobody streaming" "$(post /api/buzz '{"device":"Nobody","controller":0}' | json status)" "False"

# The updater: this build is newer than any published release, so a real check against GitHub must
# find the latest release, read it and decide there is nothing to install.
expect "updater is idle at start" "$(get /api/update | json stage)" "idle"
expect "updater starts a check" "$(post /api/update '{}' | json started)" "True"
stage=checking
for _ in $(seq 1 30); do
  stage=$(get /api/update | json stage)
  [ "$stage" = checking ] || break
  sleep 2
done
expect "updater finds no newer release than this build" "$stage $(get /api/update | json error)" "up_to_date"

# Download the latest published installer and check it against GitHub's checksum, without installing it.
# This is the path a real update takes, so a broken download or checksum check fails the build.
expect "updater starts a download check" "$(post /api/update '{"verifyOnly":true}' | json started)" "True"
stage=checking
for _ in $(seq 1 90); do
  stage=$(get /api/update | json stage)
  case "$stage" in checking|downloading) sleep 2 ;; *) break ;; esac
done
expect "latest installer downloads and matches its published checksum" "$stage $(get /api/update | json error)" "verified"

# The program must still be alive after all that
if tasklist //FI "IMAGENAME eq tunastream.exe" 2>/dev/null | grep -qi "tunastream.exe"; then pass "program is still running"; else fail "program is still running"; fi

echo "--- log lines at warning level or above (expected on a machine with no GPU or drivers) ---"
log=$(find "$app_dir" -name "tunastream.log" | head -n 1)
[ -n "$log" ] && grep -E "Warning:|Error:|Fatal:" "$log" | cut -c1-220 | head -n 25

taskkill //F //IM tunastream.exe > /dev/null 2>&1

echo "RESULT: $failures failure(s)"
[ "$failures" = "0" ]
